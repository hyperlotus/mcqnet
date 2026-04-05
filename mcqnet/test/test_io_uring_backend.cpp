#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <mcqnet/mcqnet.h>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << message << '\n';
    std::abort();
}

void check(bool condition, std::string_view message) {
    if ( !condition ) {
        fail(message);
    }
}

#if MCQNET_PLATFORM_LINUX && MCQNET_HAS_LIBURING

using mcqnet::AcceptOperation;
using mcqnet::AcceptResult;
using mcqnet::CancelSource;
using mcqnet::CompletionBackend;
using mcqnet::ConnectOperation;
using mcqnet::ConnectResult;
using mcqnet::IoUringBackend;
using mcqnet::ReadOperation;
using mcqnet::RuntimeException;
using mcqnet::SocketAddress;
using mcqnet::SocketIoResult;
using mcqnet::SocketShutdownMode;
using mcqnet::Task;
using mcqnet::TcpListener;
using mcqnet::TcpStream;
using mcqnet::WriteOperation;
using mcqnet::buffer;
using mcqnet::errc;
using mcqnet::net::AddressFamily;
using mcqnet::runtime::Runtime;
using namespace std::chrono_literals;

constexpr std::string_view kSyncPayload = "sync-io";
constexpr std::string_view kAsyncPayload = "io-uring";

struct ConnectedPair {
    TcpStream client { };
    TcpStream server { };
};

struct AsyncServerOutcome {
    AcceptResult accepted { };
    std::string payload { };
    SocketIoResult eof_result { };
};

struct AsyncClientOutcome {
    ConnectResult connect_result { };
    std::size_t total_written { 0 };
};

struct StopLatch {
    Runtime* runtime { nullptr };
    int remaining { 0 };

    void arrive() {
        MCQNET_ASSERT(runtime != nullptr);
        MCQNET_ASSERT(remaining > 0);
        --remaining;
        if ( remaining == 0 ) {
            runtime->stop();
        }
    }
};

AcceptResult wait_for_accept(TcpListener& listener) {
    for ( int attempt = 0; attempt < 2000; ++attempt ) {
        AcceptResult accepted = listener.accept_raw();
        if ( accepted.success() ) {
            return accepted;
        }
        check(accepted.would_block(), "accept_raw() should only yield would_block before a peer connects");
        std::this_thread::sleep_for(1ms);
    }
    fail("timed out while waiting for accept()");
}

void write_all(TcpStream& stream, std::string_view payload) {
    std::size_t total_written = 0;
    for ( int attempt = 0; attempt < 2000 && total_written < payload.size(); ++attempt ) {
        const SocketIoResult result = stream.write_some(
            mcqnet::net::ConstBuffer { payload.data() + total_written, payload.size() - total_written });
        if ( result.success() ) {
            check(result.transferred != 0, "write_some() should not report zero-byte progress");
            total_written += result.transferred;
            continue;
        }
        check(result.would_block(), "write_some() should only report would_block while the socket is still connecting");
        std::this_thread::sleep_for(1ms);
    }

    check(total_written == payload.size(), "write_all() should eventually send the entire payload");
}

std::string read_exact(TcpStream& stream, std::size_t expected_size) {
    std::string payload(expected_size, '\0');
    std::size_t total_read = 0;

    for ( int attempt = 0; attempt < 2000 && total_read < expected_size; ++attempt ) {
        const SocketIoResult result = stream.read_some(
            mcqnet::net::MutableBuffer { payload.data() + total_read, expected_size - total_read });
        if ( result.success() ) {
            check(result.transferred != 0, "read_some() should not report zero-byte progress before EOF");
            total_read += result.transferred;
            continue;
        }
        check(result.would_block(), "read_some() should only report would_block while waiting for peer data");
        std::this_thread::sleep_for(1ms);
    }

    check(total_read == expected_size, "read_exact() should eventually receive the entire payload");
    return payload;
}

void wait_for_eof(TcpStream& stream) {
    char byte = '\0';
    for ( int attempt = 0; attempt < 2000; ++attempt ) {
        const SocketIoResult result = stream.read_some(mcqnet::net::MutableBuffer { &byte, 1 });
        if ( result.eof() ) {
            return;
        }
        check(result.would_block(), "read_some() should reach EOF after the peer shuts down its send side");
        std::this_thread::sleep_for(1ms);
    }
    fail("timed out while waiting for EOF");
}

ConnectedPair open_connected_pair(
    mcqnet::runtime::Handle client_runtime_handle = {}, mcqnet::runtime::Handle server_runtime_handle = {}) {
    TcpListener listener = TcpListener::open(AddressFamily::ipv4, server_runtime_handle);
    listener.set_reuse_address(true);
    listener.bind(SocketAddress::ipv4_loopback(0));
    listener.listen();

    const SocketAddress local_address = listener.local_address();
    TcpStream client = TcpStream::open(AddressFamily::ipv4, client_runtime_handle);
    const ConnectResult connect_result = client.connect(local_address);
    check(connect_result.success() || connect_result.in_progress(), "connect() should either finish immediately or enter in-progress");

    const AcceptResult accepted = wait_for_accept(listener);
    check(accepted.success(), "accept_raw() should eventually accept the client socket");
    check(accepted.peer_address.valid(), "accept_raw() should preserve the peer address");

    const mcqnet::error_code listener_close_error = listener.close();
    check(!listener_close_error, "closing the listener should succeed");

    return ConnectedPair {
        std::move(client),
        TcpStream(accepted.socket, server_runtime_handle)
    };
}

Task<void> async_server(
    Runtime* runtime, TcpListener* listener, std::size_t expected_size, AsyncServerOutcome* outcome, StopLatch* latch) {
    struct ArriveOnExit {
        StopLatch* latch { nullptr };

        ~ArriveOnExit() {
            if ( latch != nullptr ) {
                latch->arrive();
            }
        }
    } arrive_on_exit { latch };

    AcceptOperation accept(*listener);
    outcome->accepted = co_await accept;

    TcpStream server_stream(outcome->accepted.socket, runtime->handle());
    std::string payload(expected_size, '\0');
    std::size_t total_read = 0;

    while ( total_read < expected_size ) {
        ReadOperation read(
            server_stream,
            mcqnet::net::MutableBuffer { payload.data() + total_read, expected_size - total_read });
        const SocketIoResult result = co_await read;
        check(result.success(), "async read should make forward progress until the full payload is received");
        total_read += result.transferred;
    }

    payload.resize(total_read);
    outcome->payload = std::move(payload);

    char eof_byte = '\0';
    ReadOperation eof_read(server_stream, mcqnet::net::MutableBuffer { &eof_byte, 1 });
    outcome->eof_result = co_await eof_read;

    const mcqnet::error_code close_error = server_stream.close();
    check(!close_error, "closing the accepted server stream should succeed");
    co_return;
}

Task<void> async_client(
    Runtime* runtime, SocketAddress remote_address, std::string_view payload, AsyncClientOutcome* outcome, StopLatch* latch) {
    struct ArriveOnExit {
        StopLatch* latch { nullptr };

        ~ArriveOnExit() {
            if ( latch != nullptr ) {
                latch->arrive();
            }
        }
    } arrive_on_exit { latch };

    TcpStream client = TcpStream::open(AddressFamily::ipv4, runtime->handle());
    ConnectOperation connect(client, remote_address);
    outcome->connect_result = co_await connect;

    while ( outcome->total_written < payload.size() ) {
        WriteOperation write(
            client,
            mcqnet::net::ConstBuffer { payload.data() + outcome->total_written, payload.size() - outcome->total_written });
        const SocketIoResult result = co_await write;
        check(result.success(), "async write should make forward progress until the full payload is sent");
        outcome->total_written += result.transferred;
    }

    client.shutdown(SocketShutdownMode::send);
    const mcqnet::error_code close_error = client.close();
    check(!close_error, "closing the async client stream should succeed");
    co_return;
}

Task<void> cancelled_read(Runtime* runtime, TcpStream* stream, mcqnet::CancelToken cancel_token, bool* cancelled) {
    char storage[16] { };

    try {
        ReadOperation read(*stream, buffer(storage), {}, std::move(cancel_token));
        (void)co_await read;
    } catch ( const RuntimeException& ex ) {
        *cancelled = ex.code().value == errc::operation_aborted;
    }

    runtime->stop();
}

#endif

} // namespace

int main() {
#if MCQNET_PLATFORM_LINUX && MCQNET_HAS_LIBURING
    {
        TcpListener listener = TcpListener::open(AddressFamily::ipv4);
        listener.set_reuse_address(true);
        listener.bind(SocketAddress::ipv4_loopback(0));
        listener.listen();

        const SocketAddress local_address = listener.local_address();
        check(local_address.valid(), "local_address() should return a valid loopback address");
        check(local_address.port() != 0, "binding to port 0 should yield an ephemeral local port");

        const AcceptResult initial_accept = listener.accept_raw();
        check(initial_accept.would_block(), "accept_raw() should surface the initial would_block state");

        TcpStream client = TcpStream::open(AddressFamily::ipv4);
        const ConnectResult connect_result = client.connect(local_address);
        check(connect_result.success() || connect_result.in_progress(), "connect() should preserve non-blocking connect state");

        const AcceptResult accepted = wait_for_accept(listener);
        check(accepted.success(), "listener should eventually accept the loopback client");

        TcpStream server(accepted.socket);
        check(server.peer_address().valid(), "peer_address() should work on an accepted socket");
        check(client.local_address().valid(), "local_address() should work on the connecting client socket");

        write_all(client, kSyncPayload);
        check(read_exact(server, kSyncPayload.size()) == kSyncPayload, "synchronous read/write should preserve payload bytes");

        client.shutdown(SocketShutdownMode::send);
        wait_for_eof(server);

        check(!client.close(), "closing the synchronous client stream should succeed");
        check(!server.close(), "closing the synchronous server stream should succeed");
        check(!listener.close(), "closing the synchronous listener should succeed");
    }

    {
        Runtime runtime;
        CompletionBackend* completion_backend = runtime.completion_backend();
        check(
            completion_backend == nullptr || completion_backend->io_backend() != nullptr,
            "Runtime auto-selected backend should be IO-capable when present");

        if ( completion_backend != nullptr ) {
            TcpListener listener = TcpListener::open(AddressFamily::ipv4, runtime.handle());
            listener.set_reuse_address(true);
            listener.bind(SocketAddress::ipv4_loopback(0));
            listener.listen();

            const SocketAddress local_address = listener.local_address();
            AsyncServerOutcome server_outcome { };
            AsyncClientOutcome client_outcome { };
            StopLatch latch { &runtime, 2 };
            auto server = runtime.spawn(async_server(&runtime, &listener, kAsyncPayload.size(), &server_outcome, &latch));
            auto client = runtime.spawn(async_client(&runtime, local_address, kAsyncPayload, &client_outcome, &latch));

            runtime.run();
            server.get();
            client.get();

            check(server_outcome.accepted.success(), "auto-selected io_uring accept should complete with a valid socket");
            check(server_outcome.payload == kAsyncPayload, "auto-selected io_uring read should preserve payload bytes");
            check(server_outcome.eof_result.eof(), "auto-selected io_uring read should surface EOF after client shutdown");
            check(client_outcome.connect_result.success(), "auto-selected io_uring connect should finish successfully");
            check(client_outcome.total_written == kAsyncPayload.size(), "auto-selected io_uring write should send the full payload");

            check(!listener.close(), "closing the async listener should succeed");
        }
    }

    {
        IoUringBackend backend;
        Runtime runtime(&backend);
        ConnectedPair pair = open_connected_pair({}, runtime.handle());
        CancelSource cancel_source;
        bool cancelled = false;

        auto task = cancelled_read(&runtime, &pair.server, cancel_source.token(), &cancelled);
        task.start();

        std::thread canceller([cancel_source]() mutable {
            std::this_thread::sleep_for(20ms);
            (void)cancel_source.cancel();
        });

        runtime.run();
        canceller.join();
        task.await_resume();

        check(cancelled, "cancelling an io_uring read should resume the waiter with operation_aborted");
        check(!pair.client.close(), "closing the cancellation test client stream should succeed");
        check(!pair.server.close(), "closing the cancellation test server stream should succeed");
    }
#endif

    std::cout << "io_uring backend test passed\n";
    return 0;
}
