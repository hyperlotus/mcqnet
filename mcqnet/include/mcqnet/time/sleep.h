#pragma once

// runtime 之上的 timer awaitable。
// 这层直接复用 runtime 的 scheduler/work-tracker 语义，因此 sleep 也会计入 pending work。

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/operation_base.h>
#include <mcqnet/detail/scheduler.h>
#include <mcqnet/runtime/cancel.h>
#include <mcqnet/runtime/runtime.h>

namespace mcqnet::time::detail {

enum class TimerMode : std::uint8_t { sleep = 0, timeout = 1 };

struct RuntimeTimerAccess {
    MCQNET_NODISCARD
    static inline mcqnet::detail::SchedulerBinding binding(const runtime::Handle& handle) noexcept {
        if ( !handle.valid() ) {
            return { };
        }

        return mcqnet::detail::SchedulerBinding {
            &runtime::Runtime::schedule_ready_continuation,
            handle.runtime_,
            &runtime::Runtime::retain_pending_work,
            &runtime::Runtime::release_pending_work
        };
    }

    MCQNET_NODISCARD
    static inline std::uint64_t schedule(
        const runtime::Handle& handle, runtime::Runtime::time_point deadline, void (*callback)(void*) noexcept, void* context) {
        MCQNET_ASSERT(handle.runtime_ != nullptr);
        return handle.runtime_->schedule_timer(callback, context, deadline);
    }

    inline static bool cancel(const runtime::Handle& handle, std::uint64_t timer_id) noexcept {
        if ( handle.runtime_ == nullptr || timer_id == 0 ) {
            return false;
        }
        return handle.runtime_->cancel_timer(timer_id);
    }
};

template <TimerMode TMode> class BasicTimerOperation final : public mcqnet::detail::OperationBase {
public:
    BasicTimerOperation(runtime::Handle runtime_handle, runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token)
        : runtime_handle_(runtime_handle)
        , deadline_(deadline)
        , cancel_token_(std::move(cancel_token)) { }

    BasicTimerOperation(const BasicTimerOperation&) = delete;
    BasicTimerOperation& operator=(const BasicTimerOperation&) = delete;

    ~BasicTimerOperation() {
        cancel_registration_.reset();
        const std::uint64_t timer_id = timer_id_.exchange(0, std::memory_order_acq_rel);
        if ( runtime_handle_.valid() && timer_id != 0 ) {
            RuntimeTimerAccess::cancel(runtime_handle_, timer_id);
        }
    }

    inline void bind_explicit_runtime_if_missing() noexcept {
        mcqnet::detail::bind_scheduler_if_missing(*this, RuntimeTimerAccess::binding(runtime_handle_));
    }

    inline void submit() {
        if ( !runtime_handle_.valid() ) {
            runtime_handle_ = runtime::Runtime::current_handle();
            bind_explicit_runtime_if_missing();
        }
        if ( !runtime_handle_.valid() ) {
            core::throw_runtime_error(
                core::error_code { core::errc::runtime_not_initialized },
                TMode == TimerMode::sleep ? "time::sleep_* requires a runtime" : "time::timeout() requires a runtime");
        }

        if ( cancel_token_.valid() ) {
            cancel_registration_.reset(cancel_token_, &BasicTimerOperation::cancel_from_token, this);
            if ( resolved_.load(std::memory_order_acquire) ) {
                return;
            }
        }

        if ( deadline_ <= runtime::Runtime::clock::now() ) {
            finish_from_deadline();
            return;
        }

        const std::uint64_t timer_id = RuntimeTimerAccess::schedule(runtime_handle_, deadline_, &BasicTimerOperation::fire_from_runtime, this);
        timer_id_.store(timer_id, std::memory_order_release);

        if ( resolved_.load(std::memory_order_acquire) ) {
            RuntimeTimerAccess::cancel(runtime_handle_, timer_id);
            timer_id_.store(0, std::memory_order_release);
        }
    }

    inline void await_resume() {
        rethrow_if_exception();
        if ( state() != mcqnet::detail::OperationState::cancelled ) {
            return;
        }

        const core::errc error = completion_error() == 0 ? core::errc::cancelled
                                                         : static_cast<core::errc>(completion_error());
        core::throw_runtime_error(
            core::error_code { error },
            error == core::errc::timed_out ? "time::timeout() expired" : "time awaitable was cancelled");
    }

private:
    static inline void fire_from_runtime(void* context) noexcept {
        MCQNET_ASSERT(context != nullptr);
        static_cast<BasicTimerOperation*>(context)->finish_from_deadline();
    }

    static inline void cancel_from_token(void* context) noexcept {
        MCQNET_ASSERT(context != nullptr);
        static_cast<BasicTimerOperation*>(context)->cancel_from_source();
    }

    inline void finish_from_deadline() noexcept {
        if ( resolved_.exchange(true, std::memory_order_acq_rel) ) {
            return;
        }

        cancel_registration_.reset();
        timer_id_.store(0, std::memory_order_release);
        if constexpr ( TMode == TimerMode::sleep ) {
            complete(0);
        } else {
            cancel(static_cast<std::int32_t>(core::errc::timed_out));
        }
    }

    inline void cancel_from_source() noexcept {
        if ( resolved_.exchange(true, std::memory_order_acq_rel) ) {
            return;
        }

        const std::uint64_t timer_id = timer_id_.exchange(0, std::memory_order_acq_rel);
        if ( runtime_handle_.valid() && timer_id != 0 ) {
            RuntimeTimerAccess::cancel(runtime_handle_, timer_id);
        }
        cancel(static_cast<std::int32_t>(core::errc::operation_aborted));
    }

private:
    runtime::Handle runtime_handle_ { };
    runtime::Runtime::time_point deadline_ { };
    runtime::CancelToken cancel_token_ { };
    runtime::CancelRegistration cancel_registration_ { };
    std::atomic<std::uint64_t> timer_id_ { 0 };
    std::atomic<bool> resolved_ { false };
};

template <TimerMode TMode> class BasicTimerAwaitable {
public:
    BasicTimerAwaitable(runtime::Handle runtime_handle, runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token)
        : operation_(runtime_handle, deadline, std::move(cancel_token)) { }

    BasicTimerAwaitable(const BasicTimerAwaitable&) = delete;
    BasicTimerAwaitable& operator=(const BasicTimerAwaitable&) = delete;
    BasicTimerAwaitable(BasicTimerAwaitable&&) noexcept = default;
    BasicTimerAwaitable& operator=(BasicTimerAwaitable&&) noexcept = default;

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept { return operation_.is_completed(); }

    inline bool await_suspend(std::coroutine_handle<> continuation) {
        operation_.bind_explicit_runtime_if_missing();
        mcqnet::detail::bind_current_scheduler_if_missing(operation_);
        if ( !operation_.prepare_await(continuation) ) {
            return false;
        }

        operation_.retain_work();
        try {
            operation_.submit();
            return true;
        } catch ( ... ) {
            operation_.release_work();
            operation_.complete_inline_exception(std::current_exception());
            return false;
        }
    }

    inline void await_resume() { operation_.await_resume(); }

private:
    BasicTimerOperation<TMode> operation_;
};

template <typename TDuration>
MCQNET_NODISCARD inline runtime::Runtime::time_point deadline_after(TDuration duration) noexcept {
    const auto delta = std::chrono::duration_cast<runtime::Runtime::clock::duration>(duration);
    return runtime::Runtime::clock::now() + delta;
}

} // namespace mcqnet::time::detail

namespace mcqnet::time {

template <typename TDuration>
MCQNET_NODISCARD inline auto sleep_for(TDuration duration, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::sleep> {
        runtime::Handle { }, detail::deadline_after(duration), std::move(cancel_token)
    };
}

template <typename TDuration>
MCQNET_NODISCARD inline auto sleep_for(runtime::Handle runtime_handle, TDuration duration, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::sleep> {
        runtime_handle, detail::deadline_after(duration), std::move(cancel_token)
    };
}

MCQNET_NODISCARD inline auto sleep_until(
    runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::sleep> {
        runtime::Handle { }, deadline, std::move(cancel_token)
    };
}

MCQNET_NODISCARD inline auto sleep_until(
    runtime::Handle runtime_handle, runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::sleep> {
        runtime_handle, deadline, std::move(cancel_token)
    };
}

} // namespace mcqnet::time

namespace mcqnet {

using time::sleep_for;
using time::sleep_until;

} // namespace mcqnet
