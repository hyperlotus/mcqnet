#pragma once

#include <mcqnet/config/feature.h>

#if MCQNET_PLATFORM_LINUX && MCQNET_HAS_LIBURING

#include <liburing.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/net/linux_socket_api.h>
#include <mcqnet/net/socket_operations.h>
#include <mcqnet/runtime/completion_backend.h>
#include <mcqnet/runtime/io_backend.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcqnet::runtime {

class IoUringBackend final : public CompletionBackend, public IoBackend {
public:
    explicit IoUringBackend(unsigned queue_depth = 256)
        : queue_depth_(queue_depth == 0 ? 256U : queue_depth) {
        wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if ( wake_fd_ < 0 ) {
            core::throw_runtime_error(
                core::error_code { core::errc::uring_error, static_cast<std::uint32_t>(errno) },
                "IoUringBackend failed to create wake eventfd");
        }

        const int init_result = ::io_uring_queue_init(queue_depth_, &ring_, 0);
        if ( init_result < 0 ) {
            const int native_error = -init_result;
            ::close(wake_fd_);
            wake_fd_ = -1;
            core::throw_runtime_error(
                core::error_code { core::errc::uring_error, static_cast<std::uint32_t>(native_error) },
                "IoUringBackend failed to initialize io_uring");
        }

        ring_initialized_ = true;
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            ensure_wake_poll_armed_locked();
            submit_ring_locked();
        }
    }

    IoUringBackend(const IoUringBackend&) = delete;
    IoUringBackend& operator=(const IoUringBackend&) = delete;

    ~IoUringBackend() override {
        if ( ring_initialized_ ) {
            ::io_uring_queue_exit(&ring_);
            ring_initialized_ = false;
        }
        if ( wake_fd_ >= 0 ) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
    }

    MCQNET_NODISCARD
    IoBackend* io_backend() noexcept override { return this; }

    void submit(IoOperationBase& operation) override {
        const std::uint64_t request_id = allocate_request_id();
        auto entry = std::make_unique<OperationEntry>();
        entry->id = request_id;
        entry->operation = &operation;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ids_by_operation_.emplace(&operation, request_id);
            entries_by_id_.emplace(request_id, std::move(entry));
        }

        try {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            OperationEntry& stored_entry = require_entry(request_id);
            prepare_operation_submission_locked(stored_entry);
            submit_ring_locked();
        } catch ( ... ) {
            erase_entry(request_id);
            throw;
        }
    }

    void cancel(IoOperationBase& operation) noexcept override {
        bool should_wake = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto operation_id = ids_by_operation_.find(&operation);
            if ( operation_id != ids_by_operation_.end() ) {
                auto entry = entries_by_id_.find(operation_id->second);
                if ( entry != entries_by_id_.end() && !entry->second->cancel_requested ) {
                    entry->second->cancel_requested = true;
                    pending_cancel_ids_.push_back(entry->second->id);
                    should_wake = true;
                }
            }
        }

        if ( should_wake ) {
            wake();
        }
    }

    bool poll(clock::duration timeout) override {
        const bool non_blocking = timeout == clock::duration::zero();
        const bool infinite = timeout == clock::duration::max();
        const auto deadline = infinite ? clock::time_point::max() : (clock::now() + timeout);

        for ( ;; ) {
            flush_pending_cancel_submissions();

            const int ready_completions = drain_ready_cqes();
            if ( ready_completions > 0 ) {
                return true;
            }
            if ( non_blocking ) {
                return false;
            }

            io_uring_cqe* cqe = nullptr;
            const int wait_result = wait_for_one_cqe(deadline, infinite, &cqe);
            if ( wait_result == -ETIME || wait_result == -EAGAIN ) {
                return false;
            }
            if ( wait_result < 0 ) {
                const int native_error = -wait_result;
                core::throw_runtime_error(
                    core::error_code { core::errc::uring_error, static_cast<std::uint32_t>(native_error) },
                    "IoUringBackend::poll() failed while waiting for a CQE");
            }

            int completed = process_cqe(cqe);
            ::io_uring_cqe_seen(&ring_, cqe);
            completed += drain_ready_cqes();
            if ( completed > 0 ) {
                return true;
            }

            if ( !infinite && clock::now() >= deadline ) {
                return false;
            }
        }
    }

    void wake() noexcept override {
        if ( wake_fd_ < 0 ) {
            return;
        }

        const std::uint64_t wake_value = 1;
        for ( ;; ) {
            const ssize_t written = ::write(wake_fd_, &wake_value, sizeof(wake_value));
            if ( written == static_cast<ssize_t>(sizeof(wake_value)) ) {
                return;
            }
            if ( written < 0 && errno == EINTR ) {
                continue;
            }
            return;
        }
    }

private:
    struct OperationEntry {
        std::uint64_t id { 0 };
        IoOperationBase* operation { nullptr };
        bool cancel_requested { false };
        sockaddr_storage accept_address { };
        SocketAddress::native_size_type accept_address_length { 0 };
        SocketAddress connect_address { };
    };

    static constexpr std::uint64_t kCancelUserDataMask = (std::uint64_t { 1 } << 63);
    static constexpr std::uint64_t kWakeUserData = std::numeric_limits<std::uint64_t>::max();

    MCQNET_NODISCARD
    std::uint64_t allocate_request_id() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if ( next_request_id_ >= kCancelUserDataMask ) {
            core::throw_runtime_error(
                core::error_code { core::errc::invalid_state },
                "IoUringBackend ran out of request ids");
        }
        return next_request_id_++;
    }

    MCQNET_NODISCARD
    OperationEntry& require_entry(std::uint64_t request_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto entry = entries_by_id_.find(request_id);
        MCQNET_ASSERT(entry != entries_by_id_.end());
        return *entry->second;
    }

    void erase_entry(std::uint64_t request_id) noexcept {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto entry = entries_by_id_.find(request_id);
        if ( entry == entries_by_id_.end() ) {
            return;
        }
        if ( entry->second->operation != nullptr ) {
            ids_by_operation_.erase(entry->second->operation);
        }
        entries_by_id_.erase(entry);
    }

    MCQNET_NODISCARD
    std::unique_ptr<OperationEntry> take_entry(std::uint64_t request_id) noexcept {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto entry = entries_by_id_.find(request_id);
        if ( entry == entries_by_id_.end() ) {
            return nullptr;
        }
        if ( entry->second->operation != nullptr ) {
            ids_by_operation_.erase(entry->second->operation);
        }
        auto owned_entry = std::move(entry->second);
        entries_by_id_.erase(entry);
        return owned_entry;
    }

    void flush_pending_cancel_submissions() {
        std::vector<std::uint64_t> cancel_ids;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if ( pending_cancel_ids_.empty() ) {
                return;
            }
            cancel_ids.swap(pending_cancel_ids_);
        }

        std::lock_guard<std::mutex> lock(ring_mutex_);
        for ( const std::uint64_t cancel_id : cancel_ids ) {
            io_uring_sqe* sqe = acquire_sqe_locked();
            ::io_uring_prep_cancel64(sqe, cancel_id, 0);
            sqe->user_data = encode_cancel_user_data(cancel_id);
        }
        submit_ring_locked();
    }

    int drain_ready_cqes() {
        int completed = 0;
        io_uring_cqe* cqe = nullptr;
        while ( ::io_uring_peek_cqe(&ring_, &cqe) == 0 ) {
            completed += process_cqe(cqe);
            ::io_uring_cqe_seen(&ring_, cqe);
            cqe = nullptr;
        }
        return completed;
    }

    MCQNET_NODISCARD
    int wait_for_one_cqe(clock::time_point deadline, bool infinite, io_uring_cqe** cqe) {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        ensure_wake_poll_armed_locked();
        submit_ring_locked();

        if ( infinite ) {
            return ::io_uring_wait_cqe(&ring_, cqe);
        }

        const clock::duration remaining = deadline <= clock::now() ? clock::duration::zero() : (deadline - clock::now());
        auto timeout = to_kernel_timespec(remaining);
        return ::io_uring_wait_cqe_timeout(&ring_, cqe, &timeout);
    }

    int process_cqe(const io_uring_cqe* cqe) {
        if ( cqe == nullptr ) {
            return 0;
        }

        if ( cqe->user_data == kWakeUserData ) {
            handle_wake_cqe();
            return 0;
        }

        if ( is_cancel_user_data(cqe->user_data) ) {
            return 0;
        }

        auto entry = take_entry(cqe->user_data);
        if ( entry == nullptr || entry->operation == nullptr ) {
            return 0;
        }

        complete_operation(*entry, cqe->res);
        return 1;
    }

    void handle_wake_cqe() noexcept {
        drain_wake_fd();
        std::lock_guard<std::mutex> lock(ring_mutex_);
        wake_poll_armed_ = false;
    }

    void complete_operation(OperationEntry& entry, int result) {
        MCQNET_ASSERT(entry.operation != nullptr);
        IoOperationBase& operation = *entry.operation;

        switch ( operation.kind() ) {
            case IoOperationKind::accept:
                complete_accept(static_cast<net::AcceptOperation&>(operation), entry, result);
                return;
            case IoOperationKind::connect:
                complete_connect(static_cast<net::ConnectOperation&>(operation), entry, result);
                return;
            case IoOperationKind::receive:
                complete_receive(static_cast<net::ReadOperation&>(operation), entry, result);
                return;
            case IoOperationKind::send:
                complete_send(static_cast<net::WriteOperation&>(operation), entry, result);
                return;
            default:
                operation.finish_cancelled(static_cast<std::int32_t>(core::errc::not_supported));
                return;
        }
    }

    void complete_accept(net::AcceptOperation& operation, OperationEntry& entry, int result) {
        if ( result >= 0 ) {
            operation.complete_accept(AcceptResult {
                SocketHandle { result },
                SocketAddress::from_native(
                    reinterpret_cast<const sockaddr*>(&entry.accept_address), entry.accept_address_length),
                { }
            });
            return;
        }

        const int native_error = -result;
        if ( entry.cancel_requested && native_error == ECANCELED ) {
            operation.finish_cancelled(static_cast<std::int32_t>(core::errc::operation_aborted));
            return;
        }

        operation.complete_accept(AcceptResult {
            { },
            { },
            net::linux_detail::LinuxSocketApi::error_code_from_errno(native_error)
        });
    }

    void complete_connect(net::ConnectOperation& operation, OperationEntry&, int result) {
        if ( result < 0 ) {
            const int native_error = -result;
            if ( native_error == ECANCELED ) {
                operation.finish_cancelled(static_cast<std::int32_t>(core::errc::operation_aborted));
                return;
            }

            operation.complete_connect(
                ConnectResult { false, net::linux_detail::LinuxSocketApi::error_code_from_errno(native_error) });
            return;
        }

        int socket_error = 0;
        socklen_t socket_error_length = static_cast<socklen_t>(sizeof(socket_error));
        if ( ::getsockopt(
                 operation.socket().native_handle(),
                 SOL_SOCKET,
                 SO_ERROR,
                 &socket_error,
                 &socket_error_length)
             < 0 ) {
            operation.complete_connect(
                ConnectResult { false, net::linux_detail::LinuxSocketApi::error_code_from_errno(errno) });
            return;
        }

        if ( socket_error != 0 ) {
            operation.complete_connect(
                ConnectResult { false, net::linux_detail::LinuxSocketApi::error_code_from_errno(socket_error) });
            return;
        }

        operation.complete_connect(ConnectResult { true, { } });
    }

    void complete_receive(net::ReadOperation& operation, OperationEntry& entry, int result) {
        if ( result > 0 ) {
            operation.complete_read(SocketIoResult { static_cast<std::size_t>(result), { } });
            return;
        }
        if ( result == 0 ) {
            operation.complete_read(SocketIoResult { 0, core::error_code { core::errc::end_of_file } });
            return;
        }

        const int native_error = -result;
        if ( entry.cancel_requested && native_error == ECANCELED ) {
            operation.finish_cancelled(static_cast<std::int32_t>(core::errc::operation_aborted));
            return;
        }

        operation.complete_read(
            SocketIoResult { 0, net::linux_detail::LinuxSocketApi::error_code_from_errno(native_error) });
    }

    void complete_send(net::WriteOperation& operation, OperationEntry& entry, int result) {
        if ( result >= 0 ) {
            operation.complete_write(SocketIoResult { static_cast<std::size_t>(result), { } });
            return;
        }

        const int native_error = -result;
        if ( entry.cancel_requested && native_error == ECANCELED ) {
            operation.finish_cancelled(static_cast<std::int32_t>(core::errc::operation_aborted));
            return;
        }

        operation.complete_write(
            SocketIoResult { 0, net::linux_detail::LinuxSocketApi::error_code_from_errno(native_error) });
    }

    void prepare_operation_submission_locked(OperationEntry& entry) {
        MCQNET_ASSERT(entry.operation != nullptr);

        io_uring_sqe* sqe = acquire_sqe_locked();
        switch ( entry.operation->kind() ) {
            case IoOperationKind::accept: {
                auto* operation = dynamic_cast<net::AcceptOperation*>(entry.operation);
                if ( operation == nullptr ) {
                    core::throw_runtime_error(
                        core::error_code { core::errc::not_supported },
                        "IoUringBackend only supports socket accept operations");
                }
                entry.accept_address_length = static_cast<SocketAddress::native_size_type>(sizeof(entry.accept_address));
                ::io_uring_prep_accept(
                    sqe,
                    operation->socket().native_handle(),
                    reinterpret_cast<sockaddr*>(&entry.accept_address),
                    reinterpret_cast<socklen_t*>(&entry.accept_address_length),
                    SOCK_NONBLOCK | SOCK_CLOEXEC);
                break;
            }
            case IoOperationKind::connect: {
                auto* operation = dynamic_cast<net::ConnectOperation*>(entry.operation);
                if ( operation == nullptr ) {
                    core::throw_runtime_error(
                        core::error_code { core::errc::not_supported },
                        "IoUringBackend only supports socket connect operations");
                }
                entry.connect_address = operation->remote_address();
                ::io_uring_prep_connect(
                    sqe,
                    operation->socket().native_handle(),
                    entry.connect_address.data(),
                    entry.connect_address.size());
                break;
            }
            case IoOperationKind::receive: {
                auto* operation = dynamic_cast<net::ReadOperation*>(entry.operation);
                if ( operation == nullptr ) {
                    core::throw_runtime_error(
                        core::error_code { core::errc::not_supported },
                        "IoUringBackend only supports socket receive operations");
                }
                const net::MutableBuffer buffer = operation->buffer();
                ::io_uring_prep_recv(
                    sqe,
                    operation->socket().native_handle(),
                    buffer.data,
                    static_cast<unsigned>(buffer.size),
                    0);
                break;
            }
            case IoOperationKind::send: {
                auto* operation = dynamic_cast<net::WriteOperation*>(entry.operation);
                if ( operation == nullptr ) {
                    core::throw_runtime_error(
                        core::error_code { core::errc::not_supported },
                        "IoUringBackend only supports socket send operations");
                }
                const net::ConstBuffer buffer = operation->buffer();
                ::io_uring_prep_send(
                    sqe,
                    operation->socket().native_handle(),
                    buffer.data,
                    static_cast<unsigned>(buffer.size),
                    MSG_NOSIGNAL);
                break;
            }
            default:
                core::throw_runtime_error(
                    core::error_code { core::errc::not_supported },
                    "IoUringBackend received an unsupported operation kind");
        }

        sqe->user_data = entry.id;
    }

    MCQNET_NODISCARD
    io_uring_sqe* acquire_sqe_locked() {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if ( sqe != nullptr ) {
            return sqe;
        }

        submit_ring_locked();
        sqe = ::io_uring_get_sqe(&ring_);
        if ( sqe != nullptr ) {
            return sqe;
        }

        core::throw_runtime_error(
            core::error_code { core::errc::uring_error },
            "IoUringBackend ran out of SQEs");
    }

    void ensure_wake_poll_armed_locked() {
        if ( wake_poll_armed_ || wake_fd_ < 0 ) {
            return;
        }

        io_uring_sqe* sqe = acquire_sqe_locked();
        ::io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
        sqe->user_data = kWakeUserData;
        wake_poll_armed_ = true;
    }

    void submit_ring_locked() {
        const int submit_result = ::io_uring_submit(&ring_);
        if ( submit_result < 0 ) {
            const int native_error = -submit_result;
            core::throw_runtime_error(
                core::error_code { core::errc::uring_error, static_cast<std::uint32_t>(native_error) },
                "IoUringBackend failed to submit SQEs");
        }
    }

    void drain_wake_fd() noexcept {
        if ( wake_fd_ < 0 ) {
            return;
        }

        std::uint64_t value = 0;
        for ( ;; ) {
            const ssize_t read_size = ::read(wake_fd_, &value, sizeof(value));
            if ( read_size == static_cast<ssize_t>(sizeof(value)) ) {
                continue;
            }
            if ( read_size < 0 && errno == EINTR ) {
                continue;
            }
            return;
        }
    }

    MCQNET_NODISCARD
    static __kernel_timespec to_kernel_timespec(clock::duration timeout) noexcept {
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        const auto count = nanoseconds.count();

        __kernel_timespec kernel_timeout { };
        kernel_timeout.tv_sec = count / 1000000000LL;
        kernel_timeout.tv_nsec = count % 1000000000LL;
        return kernel_timeout;
    }

    MCQNET_NODISCARD
    static constexpr bool is_cancel_user_data(std::uint64_t user_data) noexcept {
        return (user_data & kCancelUserDataMask) != 0 && user_data != kWakeUserData;
    }

    MCQNET_NODISCARD
    static constexpr std::uint64_t encode_cancel_user_data(std::uint64_t request_id) noexcept {
        return request_id | kCancelUserDataMask;
    }

private:
    std::mutex state_mutex_;
    std::mutex ring_mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<OperationEntry>> entries_by_id_;
    std::unordered_map<IoOperationBase*, std::uint64_t> ids_by_operation_;
    std::vector<std::uint64_t> pending_cancel_ids_;
    io_uring ring_ { };
    std::uint64_t next_request_id_ { 1 };
    unsigned queue_depth_ { 256 };
    int wake_fd_ { -1 };
    bool ring_initialized_ { false };
    bool wake_poll_armed_ { false };
};

} // namespace mcqnet::runtime

namespace mcqnet::runtime::detail {

inline std::unique_ptr<CompletionBackend> make_io_uring_default_completion_backend(const RuntimeOptions& options) {
#if MCQNET_HAS_EXCEPTIONS
    try {
        return std::make_unique<IoUringBackend>(options.io_uring_queue_depth);
    } catch ( const core::RuntimeException& ) {
        return nullptr;
    }
#else
    return std::make_unique<IoUringBackend>(options.io_uring_queue_depth);
#endif
}

struct IoUringDefaultBackendRegistration {
    IoUringDefaultBackendRegistration() noexcept {
        default_completion_backend_factory = &make_io_uring_default_completion_backend;
    }
};

inline const IoUringDefaultBackendRegistration io_uring_default_backend_registration { };

} // namespace mcqnet::runtime::detail

namespace mcqnet {

using runtime::IoUringBackend;

} // namespace mcqnet

#endif
