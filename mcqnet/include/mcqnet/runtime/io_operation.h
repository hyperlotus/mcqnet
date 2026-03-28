#pragma once

// 网络 IO 操作的公共基类。
// 目标是把“runtime 绑定 / pending-work 继承 / backend submit / cooperative cancel”
// 这些跨 accept/connect/read/write 共通的约束集中到一处。

#include <atomic>
#include <cstdint>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/operation_base.h>
#include <mcqnet/detail/scheduler.h>
#include <mcqnet/net/socket_handle.h>
#include <mcqnet/runtime/cancel.h>
#include <mcqnet/runtime/io_backend.h>
#include <mcqnet/runtime/runtime.h>
#include <utility>

namespace mcqnet::runtime {

enum class IoOperationKind : std::uint8_t {
    accept = 0,
    connect = 1,
    receive = 2,
    send = 3
};

} // namespace mcqnet::runtime

namespace mcqnet::runtime::detail {

struct RuntimeIoAccess {
    MCQNET_NODISCARD
    static inline ::mcqnet::detail::SchedulerBinding binding(const runtime::Handle& handle) noexcept {
        if ( !handle.valid() ) {
            return { };
        }

        return ::mcqnet::detail::SchedulerBinding {
            &runtime::Runtime::schedule_ready_continuation,
            handle.runtime_,
            &runtime::Runtime::retain_pending_work,
            &runtime::Runtime::release_pending_work
        };
    }

    MCQNET_NODISCARD
    static inline runtime::CompletionBackend* completion_backend(const runtime::Handle& handle) noexcept {
        if ( !handle.valid() ) {
            return nullptr;
        }
        return handle.runtime_->completion_backend();
    }
};

} // namespace mcqnet::runtime::detail

namespace mcqnet::runtime {

class IoOperationBase : public ::mcqnet::detail::OperationBase {
public:
    IoOperationBase(
        IoOperationKind kind, net::SocketHandle socket, Handle runtime_handle = {}, CancelToken cancel_token = {}) noexcept
        : kind_(kind)
        , socket_(socket)
        , runtime_handle_(runtime_handle)
        , cancel_token_(std::move(cancel_token)) { }

    IoOperationBase(const IoOperationBase&) = delete;
    IoOperationBase& operator=(const IoOperationBase&) = delete;

    ~IoOperationBase() { cancel_registration_.reset(); }

    MCQNET_NODISCARD
    inline IoOperationKind kind() const noexcept { return kind_; }

    MCQNET_NODISCARD
    inline net::SocketHandle socket() const noexcept { return socket_; }

    inline void set_socket(net::SocketHandle socket) noexcept { socket_ = socket; }

    MCQNET_NODISCARD
    inline bool has_runtime_handle() const noexcept { return runtime_handle_.valid(); }

    MCQNET_NODISCARD
    inline Handle runtime_handle() const noexcept { return runtime_handle_; }

    inline void set_runtime_handle(Handle runtime_handle) noexcept { runtime_handle_ = runtime_handle; }

    MCQNET_NODISCARD
    inline bool has_cancel_token() const noexcept { return cancel_token_.valid(); }

    MCQNET_NODISCARD
    inline const CancelToken& cancel_token() const noexcept { return cancel_token_; }

    inline void set_cancel_token(CancelToken cancel_token) noexcept { cancel_token_ = std::move(cancel_token); }

    inline void bind_explicit_runtime_if_missing() noexcept {
        ::mcqnet::detail::bind_scheduler_if_missing(*this, detail::RuntimeIoAccess::binding(runtime_handle_));
    }

    inline void finish(std::uint32_t result, std::int32_t error = 0) {
        if ( resolved_.exchange(true, std::memory_order_acq_rel) ) {
            return;
        }
        complete(result, error);
    }

    inline void finish_cancelled(std::int32_t error = 0) noexcept {
        if ( resolved_.exchange(true, std::memory_order_acq_rel) ) {
            return;
        }
        cancel(error);
    }

protected:
    inline void submit_to_io_backend() {
        resolve_runtime_if_needed();
        bind_explicit_runtime_if_missing();
        io_backend_ = &require_io_backend();
        io_backend_->submit(*this);
    }

    inline void arm_cancel_registration(CancelRegistration::CallbackFn callback, void* context) {
        if ( !cancel_token_.valid() || callback == nullptr ) {
            return;
        }
        cancel_registration_.reset(cancel_token_, callback, context);
    }

    inline void reset_cancel_registration() noexcept { cancel_registration_.reset(); }

    inline void request_backend_cancel(
        std::int32_t error = static_cast<std::int32_t>(core::errc::operation_aborted)) noexcept {
        if ( resolved_.load(std::memory_order_acquire) ) {
            return;
        }
        if ( io_backend_ != nullptr ) {
            io_backend_->cancel(*this);
            return;
        }
        finish_cancelled(error);
    }

    MCQNET_NODISCARD
    inline IoBackend* io_backend() const noexcept { return io_backend_; }

private:
    inline void resolve_runtime_if_needed() {
        if ( !runtime_handle_.valid() ) {
            runtime_handle_ = Runtime::current_handle();
        }

        if ( !runtime_handle_.valid() ) {
            core::throw_runtime_error(
                core::error_code { core::errc::runtime_not_initialized }, "IO operation requires a runtime");
        }
    }

    MCQNET_NODISCARD
    inline IoBackend& require_io_backend() {
        CompletionBackend* completion_backend = detail::RuntimeIoAccess::completion_backend(runtime_handle_);
        if ( completion_backend == nullptr ) {
            core::throw_runtime_error(
                core::error_code { core::errc::not_supported }, "IO operation requires a completion backend");
        }

        IoBackend* io_backend = completion_backend->io_backend();
        if ( io_backend == nullptr ) {
            core::throw_runtime_error(
                core::error_code { core::errc::not_supported },
                "IO operation requires an IO-capable completion backend");
        }
        return *io_backend;
    }

private:
    IoOperationKind kind_ { IoOperationKind::receive };
    net::SocketHandle socket_ { };
    Handle runtime_handle_ { };
    CancelToken cancel_token_ { };
    CancelRegistration cancel_registration_ { };
    IoBackend* io_backend_ { nullptr };
    std::atomic<bool> resolved_ { false };
};

} // namespace mcqnet::runtime

namespace mcqnet {

using runtime::IoOperationBase;
using runtime::IoOperationKind;

} // namespace mcqnet
