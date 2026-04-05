#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <string_view>

#include <mcqnet/detail/operation_awaiter.h>
#include <mcqnet/mcqnet.h>

namespace {
using mcqnet::CancelSource;
using mcqnet::AcceptOperation;
using mcqnet::RuntimeException;
using mcqnet::SocketAddress;
using mcqnet::SocketHandle;
using mcqnet::SocketIoResult;
using mcqnet::SocketShutdownMode;
using mcqnet::Task;
using mcqnet::TcpListener;
using mcqnet::TcpStream;
using mcqnet::AcceptResult;
using mcqnet::ConnectOperation;
using mcqnet::ConnectResult;
using mcqnet::ReadOperation;
using mcqnet::WriteOperation;
using mcqnet::net::AddressFamily;
using mcqnet::buffer;
using mcqnet::errc;
using mcqnet::net::ConstBuffer;
using mcqnet::net::MutableBuffer;
using mcqnet::runtime::IoBackend;
using mcqnet::runtime::IoOperationBase;
using mcqnet::runtime::IoOperationKind;
using mcqnet::runtime::RuntimeBackendPolicy;
using mcqnet::runtime::RuntimeOptions;
using mcqnet::runtime::Runtime;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << message << '\n';
    std::abort();
}

void check(bool condition, std::string_view message) {
    if ( !condition ) {
        fail(message);
    }
}

class ManualIoBackend final : public mcqnet::runtime::CompletionBackend, public IoBackend {
public:
    struct ReadyCompletion {
        IoOperationBase* operation { nullptr };
        std::uint32_t result { 0 };
    };

    mcqnet::runtime::IoBackend* io_backend() noexcept override { return this; }

    void submit(IoOperationBase& operation) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(&operation);
        last_kind_ = operation.kind();
        last_socket_ = operation.socket();
    }

    void cancel(IoOperationBase& operation) noexcept override {
        operation.finish_cancelled(static_cast<std::int32_t>(errc::operation_aborted));
    }

    void complete_next(std::uint32_t result) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(!pending_.empty());
            ready_.push_back(ReadyCompletion { pending_.front(), result });
            pending_.pop_front();
        }
        cv_.notify_one();
    }

    bool poll(clock::duration timeout) override {
        ReadyCompletion completion { };
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] { return wake_requested_ || !ready_.empty(); };

            if ( ready_.empty() && !wake_requested_ ) {
                if ( timeout == clock::duration::zero() ) {
                    return false;
                }
                if ( timeout == clock::duration::max() ) {
                    cv_.wait(lock, ready);
                } else {
                    cv_.wait_for(lock, timeout, ready);
                }
            }

            if ( ready_.empty() ) {
                wake_requested_ = false;
                return false;
            }

            completion = ready_.front();
            ready_.pop_front();
            wake_requested_ = false;
        }

        completion.operation->finish(completion.result);
        return true;
    }

    void wake() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wake_requested_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] IoOperationKind last_kind() const noexcept { return last_kind_; }
    [[nodiscard]] SocketHandle last_socket() const noexcept { return last_socket_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<IoOperationBase*> pending_;
    std::deque<ReadyCompletion> ready_;
    IoOperationKind last_kind_ { IoOperationKind::receive };
    SocketHandle last_socket_ { };
    bool wake_requested_ { false };
};

class ManualIoOperation final : public IoOperationBase {
public:
    explicit ManualIoOperation(mcqnet::runtime::Handle runtime_handle, SocketHandle socket) noexcept
        : IoOperationBase(IoOperationKind::receive, socket, runtime_handle) { }

    void submit() {
        submitted_ = true;
        submit_to_io_backend();
    }

    int await_resume() {
        rethrow_if_exception();
        return completion_result();
    }

    [[nodiscard]] bool submitted() const noexcept { return submitted_; }

private:
    bool submitted_ { false };
};

class CancellableIoOperation final : public IoOperationBase {
public:
    CancellableIoOperation(mcqnet::runtime::Handle runtime_handle, SocketHandle socket, mcqnet::CancelToken cancel_token) noexcept
        : IoOperationBase(IoOperationKind::receive, socket, runtime_handle, std::move(cancel_token)) { }

    void submit() {
        submitted_ = true;
        arm_cancel_registration(&CancellableIoOperation::cancel_from_source, this);
        if ( is_completed() ) {
            return;
        }
        submit_to_io_backend();
    }

    int await_resume() {
        rethrow_if_exception();
        if ( state() == mcqnet::detail::OperationState::cancelled ) {
            mcqnet::throw_runtime_error(
                mcqnet::error_code {
                    completion_error() == 0 ? errc::cancelled : static_cast<errc>(completion_error())
                },
                "Cancellable IO operation was cancelled");
        }
        return completion_result();
    }

    [[nodiscard]] bool submitted() const noexcept { return submitted_; }

private:
    static void cancel_from_source(void* context) noexcept {
        MCQNET_ASSERT(context != nullptr);
        static_cast<CancellableIoOperation*>(context)->request_backend_cancel();
    }

private:
    bool submitted_ { false };
};

Task<void> await_manual_io_and_stop(ManualIoOperation& operation, Runtime* runtime, int* result, bool* completed) {
    *result = co_await mcqnet::detail::make_operation_awaiter(operation);
    *completed = true;
    runtime->stop();
}

Task<void> await_manual_io_expect_not_supported(ManualIoOperation& operation, bool* saw_not_supported) {
    try {
        (void)co_await mcqnet::detail::make_operation_awaiter(operation);
    } catch ( const RuntimeException& ex ) {
        *saw_not_supported = ex.code().value == errc::not_supported;
    }
}

Task<void> await_cancellable_io_and_stop(CancellableIoOperation& operation, Runtime* runtime, bool* cancelled) {
    try {
        (void)co_await mcqnet::detail::make_operation_awaiter(operation);
    } catch ( const RuntimeException& ex ) {
        *cancelled = ex.code().value == errc::operation_aborted;
    }
    runtime->stop();
}

} // namespace

int main() {
    {
        const SocketAddress ipv4 = SocketAddress::ipv4_loopback(8080);
        check(ipv4.valid(), "SocketAddress::ipv4_loopback() should create a valid IPv4 address");
        check(ipv4.family() == AddressFamily::ipv4, "IPv4 loopback should report the IPv4 family");
        check(ipv4.port() == 8080, "SocketAddress should preserve the IPv4 port");
        check(ipv4.ip_string() == "127.0.0.1", "SocketAddress should stringify IPv4 addresses");
        check(ipv4.to_string() == "127.0.0.1:8080", "SocketAddress::to_string() should include the IPv4 port");

        const SocketAddress parsed_ipv4 = SocketAddress::parse("127.0.0.1", 9001);
        check(parsed_ipv4.family() == AddressFamily::ipv4, "SocketAddress::parse() should parse IPv4 literals");
        check(parsed_ipv4.port() == 9001, "SocketAddress::parse() should preserve the requested port");
        check(
            parsed_ipv4.ipv4_bytes() == std::array<std::uint8_t, 4> { 127, 0, 0, 1 },
            "SocketAddress should preserve IPv4 bytes");

        const SocketAddress ipv6 = SocketAddress::ipv6_loopback(443);
        check(ipv6.valid(), "SocketAddress::ipv6_loopback() should create a valid IPv6 address");
        check(ipv6.family() == AddressFamily::ipv6, "IPv6 loopback should report the IPv6 family");
        check(ipv6.port() == 443, "SocketAddress should preserve the IPv6 port");
        check(ipv6.ip_string() == "::1", "SocketAddress should stringify IPv6 addresses");
        check(ipv6.to_string() == "[::1]:443", "SocketAddress::to_string() should bracket IPv6 literals");

        const SocketAddress parsed_ipv6 = SocketAddress::parse("[2001:db8::1]", 8443);
        check(parsed_ipv6.family() == AddressFamily::ipv6, "SocketAddress::parse() should parse bracketed IPv6 literals");
        check(parsed_ipv6.port() == 8443, "SocketAddress::parse() should preserve the IPv6 port");
        check(
            parsed_ipv6.to_string() == "[2001:db8::1]:8443",
            "SocketAddress::to_string() should round-trip IPv6 literals");

        const SocketAddress native_roundtrip =
            SocketAddress::from_native(parsed_ipv6.data(), parsed_ipv6.size());
        check(
            native_roundtrip == parsed_ipv6,
            "SocketAddress::from_native() should round-trip IPv6 sockaddr data");

        SocketHandle socket(41);
        check(socket.valid(), "SocketHandle should accept a native handle");
        check(socket.native_handle() == 41, "SocketHandle should preserve the native handle value");
        check(socket.release() == 41, "SocketHandle::release() should return the native handle");
        check(!socket.valid(), "SocketHandle::release() should invalidate the wrapper");

        char writable[8] { };
        const char readable[4] { 'm', 'c', 'q', 'n' };
        const MutableBuffer writable_buffer = buffer(writable);
        const ConstBuffer readable_buffer = buffer(readable);

        check(writable_buffer.size == sizeof(writable), "buffer() should preserve mutable byte size");
        check(readable_buffer.size == sizeof(readable), "buffer() should preserve const byte size");

        Runtime runtime;
        TcpStream stream(SocketHandle(7), runtime.handle());
        TcpListener listener(SocketHandle(9), runtime.handle());

        check(stream.valid(), "TcpStream should be valid when constructed from a valid socket");
        check(listener.valid(), "TcpListener should be valid when constructed from a valid socket");
        check(stream.runtime_handle().valid(), "TcpStream should carry an optional runtime binding");
        check(listener.runtime_handle().valid(), "TcpListener should carry an optional runtime binding");

        const ConnectResult connecting { false, mcqnet::error_code { errc::would_block } };
        const SocketIoResult eof_result { 0, mcqnet::error_code { errc::end_of_file } };
        const AcceptResult would_block_accept { { }, { }, mcqnet::error_code { errc::would_block } };

        check(connecting.in_progress(), "ConnectResult should expose a non-blocking in-progress state");
        check(eof_result.eof(), "SocketIoResult should expose EOF without extra decoding");
        check(would_block_accept.would_block(), "AcceptResult should expose would_block without exceptions");

#if MCQNET_PLATFORM_LINUX
        auto open_stream = static_cast<TcpStream (*)(AddressFamily, mcqnet::runtime::Handle)>(&TcpStream::open);
        auto close_stream = static_cast<mcqnet::error_code (TcpStream::*)() noexcept>(&TcpStream::close);
        auto connect_stream = static_cast<ConnectResult (TcpStream::*)(const SocketAddress&) noexcept>(&TcpStream::connect);
        auto read_stream = static_cast<SocketIoResult (TcpStream::*)(MutableBuffer) noexcept>(&TcpStream::read_some);
        auto write_stream = static_cast<SocketIoResult (TcpStream::*)(ConstBuffer) noexcept>(&TcpStream::write_some);
        auto shutdown_stream = static_cast<void (TcpStream::*)(SocketShutdownMode)>(&TcpStream::shutdown);
        auto open_listener = static_cast<TcpListener (*)(AddressFamily, mcqnet::runtime::Handle)>(&TcpListener::open);
        auto close_listener = static_cast<mcqnet::error_code (TcpListener::*)() noexcept>(&TcpListener::close);
        auto accept_listener = static_cast<AcceptResult (TcpListener::*)() const noexcept>(&TcpListener::accept_raw);

        (void)open_stream;
        (void)close_stream;
        (void)connect_stream;
        (void)read_stream;
        (void)write_stream;
        (void)shutdown_stream;
        (void)open_listener;
        (void)close_listener;
        (void)accept_listener;
#endif

        ConnectOperation connect_operation(stream, SocketAddress::ipv4_loopback(8081));
        AcceptOperation accept_operation(listener);
        ReadOperation read_operation(stream, writable_buffer);
        WriteOperation write_operation(stream, readable_buffer);

        check(
            connect_operation.kind() == IoOperationKind::connect,
            "ConnectOperation should report the connect kind through IoOperationBase");
        check(
            accept_operation.kind() == IoOperationKind::accept,
            "AcceptOperation should report the accept kind through IoOperationBase");
        check(
            read_operation.kind() == IoOperationKind::receive,
            "ReadOperation should report the receive kind through IoOperationBase");
        check(
            write_operation.kind() == IoOperationKind::send,
            "WriteOperation should report the send kind through IoOperationBase");

        check(
            connect_operation.remote_address() == SocketAddress::ipv4_loopback(8081),
            "ConnectOperation should preserve the requested remote address");
        check(
            read_operation.buffer().data == writable_buffer.data && read_operation.buffer().size == writable_buffer.size,
            "ReadOperation should preserve the mutable buffer view");
        check(
            write_operation.buffer().data == readable_buffer.data && write_operation.buffer().size == readable_buffer.size,
            "WriteOperation should preserve the const buffer view");

        connect_operation.complete_connect(ConnectResult { true, { } });
        check(
            connect_operation.result().success(),
            "ConnectOperation::complete_connect() should preserve the connect result shape");

        AcceptOperation accept_completed(listener);
        accept_completed.complete_accept(
            AcceptResult { SocketHandle(77), SocketAddress::ipv6_loopback(9000), mcqnet::error_code { } });
        check(
            accept_completed.result().success() && accept_completed.accepted_socket() == SocketHandle(77),
            "AcceptOperation::complete_accept() should preserve accepted socket state");
        check(
            accept_completed.peer_address() == SocketAddress::ipv6_loopback(9000),
            "AcceptOperation::complete_accept() should preserve peer address state");

        ReadOperation read_completed(stream, writable_buffer);
        read_completed.complete_read(SocketIoResult { 4, mcqnet::error_code { errc::end_of_file } });
        check(
            read_completed.result().eof() && read_completed.result().transferred == 4,
            "ReadOperation::complete_read() should preserve EOF and byte count");

        WriteOperation write_completed(stream, readable_buffer);
        write_completed.complete_write(SocketIoResult { 3, mcqnet::error_code { } });
        check(
            write_completed.result().success() && write_completed.result().transferred == 3,
            "WriteOperation::complete_write() should preserve byte count");

        auto connect_awaiter = connect_operation.operator co_await();
        auto accept_awaiter = accept_operation.operator co_await();
        auto read_awaiter = read_operation.operator co_await();
        auto write_awaiter = write_operation.operator co_await();

        (void)connect_awaiter;
        (void)accept_awaiter;
        (void)read_awaiter;
        (void)write_awaiter;
    }

    {
        ManualIoBackend backend;
        Runtime runtime(&backend);
        ManualIoOperation operation(runtime.handle(), SocketHandle(123));
        bool completed = false;
        int result = 0;
        auto task = await_manual_io_and_stop(operation, &runtime, &result, &completed);

        task.start();
        check(operation.submitted(), "IO operation should submit through the new backend protocol");
        check(
            backend.last_kind() == IoOperationKind::receive,
            "Backend should observe the operation kind through IoOperationBase");
        check(
            backend.last_socket() == SocketHandle(123),
            "Backend should observe the socket handle through IoOperationBase");

        backend.complete_next(321);
        check(runtime.run_one(), "run_one() should poll the IO backend and resume the waiting task");
        task.await_resume();

        check(completed, "IO completion should resume the waiting task");
        check(result == 321, "IO completion should propagate the result back to await_resume()");
    }

    {
        Runtime runtime(RuntimeOptions { RuntimeBackendPolicy::none });
        ManualIoOperation operation(runtime.handle(), SocketHandle(55));
        bool saw_not_supported = false;
        auto task = await_manual_io_expect_not_supported(operation, &saw_not_supported);

        task.start();
        task.await_resume();
        check(
            saw_not_supported,
            "IO operation submission should fail with not_supported when the runtime has no IO backend");
    }

    {
        ManualIoBackend backend;
        Runtime runtime(&backend);
        CancelSource cancel_source;
        CancellableIoOperation operation(runtime.handle(), SocketHandle(88), cancel_source.token());
        bool cancelled = false;
        auto task = await_cancellable_io_and_stop(operation, &runtime, &cancelled);

        task.start();
        check(operation.submitted(), "Cancellable IO operation should still submit through the backend");

        cancel_source.cancel();
        check(runtime.run_one(), "run_one() should execute the continuation resumed by IO cancellation");
        task.await_resume();

        check(cancelled, "Cancelling an IO operation should resume the waiter with operation_aborted");
    }

    std::cout << "net test passed\n";
    return 0;
}
