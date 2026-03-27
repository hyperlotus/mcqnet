#pragma once

// JoinHandle：等待协程完成并获取其结果的协程同步原语。
// 基于 JoinState<T> 提供 awaitable 接口和阻塞式 wait()/get() 接口。

#include "mcqnet/config/assert.h"
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mcqnet/detail/macro.h>
#include <mcqnet/detail/scheduler.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace mcqnet::task {

using ScheduleFn = detail::ScheduleFn;

template <typename T = void> class JoinHandle;

} // namespace mcqnet::task

namespace mcqnet::detail {

template <typename T> struct JoinHandleAccess;

template <typename T> class JoinState {
public:
    JoinState() = default;
    JoinState(const JoinState&) = delete;
    JoinState& operator=(const JoinState&) = delete;

    inline void set_scheduler(task::ScheduleFn schedule_fn, void* schedule_context) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        schedule_fn_ = schedule_fn;
        schedule_context_ = schedule_context;
    }

    inline void set_work_tracker(detail::RetainWorkFn retain_work_fn, detail::ReleaseWorkFn release_work_fn) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        retain_work_fn_ = retain_work_fn;
        release_work_fn_ = release_work_fn;
    }

    MCQNET_NODISCARD
    inline bool has_work_tracker() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return retain_work_fn_ != nullptr;
    }

    template <typename TValue> inline void set_value(TValue&& value) {
        std::coroutine_handle<> continuation_to_resume { };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(!ready_);
            value_.emplace(std::forward<TValue>(value));
            ready_ = true;
            continuation_to_resume = continuation_;
        }
        cv_.notify_all();
        schedule_or_resume(continuation_to_resume);
    }

    inline void set_exception(std::exception_ptr exception) noexcept {
        std::coroutine_handle<> continuation_to_resume { };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(!ready_);
            exception_ = exception;
            ready_ = true;
            continuation_to_resume = continuation_;
        }
        cv_.notify_all();
        schedule_or_resume(continuation_to_resume);
    }

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_;
    }

    MCQNET_NODISCARD
    inline bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( ready_ ) {
            return false;
        }
        // 若 JoinHandle 自身尚未被显式绑定 scheduler，则继承当前 runtime。
        // 这样在 runtime 内 await 自由 spawn() 返回的 JoinHandle 时，父协程也会回到 runtime。
        if ( schedule_fn_ == nullptr ) {
            const detail::SchedulerBinding scheduler = detail::SchedulerScope::current();
            if ( scheduler.valid() ) {
                schedule_fn_ = scheduler.schedule_fn;
                schedule_context_ = scheduler.schedule_context;
            }
        }
        if ( retain_work_fn_ == nullptr ) {
            const detail::SchedulerBinding scheduler = detail::SchedulerScope::current();
            if ( scheduler.tracks_work() ) {
                retain_work_fn_ = scheduler.retain_work_fn;
                release_work_fn_ = scheduler.release_work_fn;
            }
        }
        continuation_ = continuation;
        retain_work();
        return true;
    }

    inline T await_resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( exception_ != nullptr ) {
            std::rethrow_exception(exception_);
        }
        MCQNET_ASSERT(ready_);
        return std::move(value_.value());
    }

    inline T get() {
        wait();
        std::lock_guard<std::mutex> lock(mutex_);
        if ( exception_ != nullptr ) {
            std::rethrow_exception(exception_);
        }
        MCQNET_ASSERT(ready_);
        return std::move(*value_);
    }

    inline void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return ready_; });
    }

private:
    inline void schedule_or_resume(std::coroutine_handle<> continuation) noexcept {
        if ( continuation == nullptr ) {
            return;
        }
        if ( schedule_fn_ != nullptr ) {
            schedule_fn_(schedule_context_, continuation);
        } else {
            continuation.resume();
        }
    }

    inline void retain_work() const noexcept {
        if ( retain_work_fn_ != nullptr ) {
            retain_work_fn_(schedule_context_);
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_ { false };
    std::exception_ptr exception_ { };
    std::optional<T> value_;
    std::coroutine_handle<> continuation_ { };
    task::ScheduleFn schedule_fn_ { nullptr };
    void* schedule_context_ { nullptr };
    detail::RetainWorkFn retain_work_fn_ { nullptr };
    detail::ReleaseWorkFn release_work_fn_ { nullptr };
};

template <> class JoinState<void> {
public:
    JoinState() = default;
    JoinState(const JoinState&) = delete;
    JoinState& operator=(const JoinState&) = delete;

    inline void set_scheduler(task::ScheduleFn schedule_fn, void* schedule_context) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        schedule_fn_ = schedule_fn;
        schedule_context_ = schedule_context;
    }

    inline void set_work_tracker(detail::RetainWorkFn retain_work_fn, detail::ReleaseWorkFn release_work_fn) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        retain_work_fn_ = retain_work_fn;
        release_work_fn_ = release_work_fn;
    }

    MCQNET_NODISCARD
    inline bool has_work_tracker() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return retain_work_fn_ != nullptr;
    }

    inline void set_value() noexcept {
        std::coroutine_handle<> continuation_to_resume { };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(!ready_);
            ready_ = true;
            continuation_to_resume = continuation_;
        }
        cv_.notify_all();
        schedule_or_resume(continuation_to_resume);
    }

    inline void set_exception(std::exception_ptr exception) noexcept {
        std::coroutine_handle<> continuation_to_resume { };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(!ready_);
            exception_ = exception;
            ready_ = true;
            continuation_to_resume = continuation_;
        }
        cv_.notify_all();
        schedule_or_resume(continuation_to_resume);
    }

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_;
    }

    MCQNET_NODISCARD
    inline bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( ready_ ) {
            return false;
        }
        // void 特化同样自动继承当前 runtime 的恢复策略。
        if ( schedule_fn_ == nullptr ) {
            const detail::SchedulerBinding scheduler = detail::SchedulerScope::current();
            if ( scheduler.valid() ) {
                schedule_fn_ = scheduler.schedule_fn;
                schedule_context_ = scheduler.schedule_context;
            }
        }
        if ( retain_work_fn_ == nullptr ) {
            const detail::SchedulerBinding scheduler = detail::SchedulerScope::current();
            if ( scheduler.tracks_work() ) {
                retain_work_fn_ = scheduler.retain_work_fn;
                release_work_fn_ = scheduler.release_work_fn;
            }
        }
        continuation_ = continuation;
        retain_work();
        return true;
    }

    inline void await_resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( exception_ != nullptr ) {
            std::rethrow_exception(exception_);
        }
        MCQNET_ASSERT(ready_);
    }

    inline void get() {
        wait();
        std::lock_guard<std::mutex> lock(mutex_);
        if ( exception_ != nullptr ) {
            std::rethrow_exception(exception_);
        }
    }

    inline void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return ready_; });
    }

private:
    inline void schedule_or_resume(std::coroutine_handle<> continuation) noexcept {
        if ( continuation == nullptr ) {
            return;
        }
        if ( schedule_fn_ != nullptr ) {
            schedule_fn_(schedule_context_, continuation);
        } else {
            continuation.resume();
        }
    }

    inline void retain_work() const noexcept {
        if ( retain_work_fn_ != nullptr ) {
            retain_work_fn_(schedule_context_);
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_ { false };
    std::exception_ptr exception_ { };
    std::coroutine_handle<> continuation_ { };
    task::ScheduleFn schedule_fn_ { nullptr };
    void* schedule_context_ { nullptr };
    detail::RetainWorkFn retain_work_fn_ { nullptr };
    detail::ReleaseWorkFn release_work_fn_ { nullptr };
};

} // namespace mcqnet::detail

namespace mcqnet::task {

template <typename T> class JoinHandle {
public:
    using value_type = T;

    JoinHandle() = default;
    JoinHandle(const JoinHandle&) = default;
    JoinHandle& operator=(const JoinHandle&) = default;
    JoinHandle(JoinHandle&&) noexcept = default;
    JoinHandle& operator=(JoinHandle&&) noexcept = default;
    MCQNET_NODISCARD
    inline bool valid() const noexcept { return static_cast<bool>(state_); }

    inline void set_scheduler(ScheduleFn schedule_fn, void* schedule_context) noexcept {
        MCQNET_ASSERT(state_ != nullptr);
        state_->set_scheduler(schedule_fn, schedule_context);
    }

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept {
        MCQNET_ASSERT(state_ != nullptr);
        return state_->await_ready();
    }

    MCQNET_NODISCARD
    inline bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        MCQNET_ASSERT(state_ != nullptr);
        return state_->await_suspend(continuation);
    }

    decltype(auto) await_resume() {
        MCQNET_ASSERT(state_ != nullptr);
        return state_->await_resume();
    }

    inline void wait() {
        MCQNET_ASSERT(state_ != nullptr);
        state_->wait();
    }

    decltype(auto) get() {
        MCQNET_ASSERT(state_ != nullptr);
        return state_->get();
    }

    decltype(auto) join() { return get(); }

private:
    using state_type = detail::JoinState<T>;

    explicit JoinHandle(std::shared_ptr<state_type> state) noexcept
        : state_(std::move(state)) { }

    template <typename TValue> friend struct detail::JoinHandleAccess;

    std::shared_ptr<state_type> state_;
};

} // namespace mcqnet::task

namespace mcqnet::detail {

template <typename T> struct JoinHandleAccess {
    MCQNET_NODISCARD
    static inline ::mcqnet::task::JoinHandle<T> make(std::shared_ptr<JoinState<T>> state) noexcept {
        return ::mcqnet::task::JoinHandle<T> { std::move(state) };
    }
};

} // namespace mcqnet::detail

namespace mcqnet {

using task::ScheduleFn;

template <typename T = void> using JoinHandle = task::JoinHandle<T>;

} // namespace mcqnet
