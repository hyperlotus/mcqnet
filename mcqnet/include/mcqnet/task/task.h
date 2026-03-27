#pragma once

// 轻量级协程任务类型，支持有返回值和无返回值两种特化。

#include "mcqnet/config/compiler.h"
#include "mcqnet/memory/thread_local_pool.h"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mcqnet/detail/macro.h>
#include <mcqnet/detail/scheduler.h>
#include <memory>
#include <new>
#include <utility>

namespace mcqnet::task {

template <typename T = void> class Task;

} // namespace mcqnet::task

namespace mcqnet::detail {

template <typename T> class ResultStorage {
public:
    ResultStorage() = default;
    ResultStorage(const ResultStorage&) = delete;
    ResultStorage& operator=(const ResultStorage&) = delete;

    ~ResultStorage() { reset(); }

    template <typename... Args> inline void emplace(Args&&... args) {
        reset();
        new (storage_) T(std::forward<Args>(args)...);
        has_value_ = true;
    }

    inline void reset() noexcept {
        if ( has_value_ ) {
            value_ref().~T();
            has_value_ = false;
        }
    }

    MCQNET_NODISCARD
    inline bool has_value() const noexcept { return has_value_; }

    MCQNET_NODISCARD
    inline T& value_ref() noexcept { return *std::launder(reinterpret_cast<T*>(storage_)); }

    inline const T& value_ref() const noexcept { return *std::launder(reinterpret_cast<T*>(storage_)); }

private:
    alignas(T) std::uint8_t storage_[sizeof(T)];
    bool has_value_ { false };
};

template <typename T> class PromiseBase {
public:
    PromiseBase() = default;
    PromiseBase(const PromiseBase&) = delete;
    PromiseBase& operator=(const PromiseBase&) = delete;

    inline std::suspend_always initial_suspend() noexcept { return { }; }

    struct FinalAwaiter {
        MCQNET_NODISCARD
        inline bool await_ready() const noexcept { return false; }

        template <typename TPromise>
        MCQNET_NODISCARD inline std::coroutine_handle<> await_suspend(
            std::coroutine_handle<TPromise> handle) const noexcept {
            auto& promise = handle.promise();
            if ( promise.continuation_ != nullptr ) {
                // 若 await 发生在 runtime 调度作用域里，则父协程恢复不直接 inline return，
                // 而是重新投递回当前 runtime 的 ready queue。
                if ( promise.schedule_fn_ != nullptr ) {
                    promise.schedule_fn_(promise.schedule_context_, promise.continuation_);
                    return std::noop_coroutine();
                }
                return promise.continuation_;
            }
            return std::noop_coroutine();
        }

        inline void await_resume() const noexcept { }
    };

    inline FinalAwaiter final_suspend() const noexcept { return { }; }

    inline void unhandled_exception() noexcept { exception_ = std::current_exception(); }

    inline void set_continuation(std::coroutine_handle<> continuation) noexcept { continuation_ = continuation; }

    // 保存“父协程应如何被恢复”的调度策略。
    // 这不影响当前 task 在哪里执行，只影响 final_suspend 时 continuation 回到哪里。
    inline void set_scheduler(detail::ScheduleFn schedule_fn, void* schedule_context) noexcept {
        schedule_fn_ = schedule_fn;
        schedule_context_ = schedule_context;
    }

    MCQNET_NODISCARD
    inline bool has_scheduler() const noexcept { return schedule_fn_ != nullptr; }

    inline void set_work_tracker(detail::RetainWorkFn retain_work_fn, detail::ReleaseWorkFn release_work_fn) noexcept {
        retain_work_fn_ = retain_work_fn;
        release_work_fn_ = release_work_fn;
    }

    MCQNET_NODISCARD
    inline bool has_work_tracker() const noexcept { return retain_work_fn_ != nullptr; }

    inline void retain_work() const noexcept {
        if ( retain_work_fn_ != nullptr ) {
            retain_work_fn_(schedule_context_);
        }
    }

    inline void release_work() const noexcept {
        if ( release_work_fn_ != nullptr ) {
            release_work_fn_(schedule_context_);
        }
    }

    MCQNET_NODISCARD
    inline std::exception_ptr exception() const noexcept { return exception_; }

    static inline void* operator new(std::size_t size) {
        return memory::ThreadLocalPool::local().allocate(size, alignof(std::max_align_t));
    }

    static inline void operator delete(void* ptr) noexcept { memory::ThreadLocalPool::local().deallocate(ptr); }

    static inline void operator delete(void* ptr, std::size_t) noexcept {
        memory::ThreadLocalPool::local().deallocate(ptr);
    }

protected:
    template <typename> friend class ::mcqnet::task::Task;

private:
    std::coroutine_handle<> continuation_ { };
    detail::ScheduleFn schedule_fn_ { nullptr };
    void* schedule_context_ { nullptr };
    detail::RetainWorkFn retain_work_fn_ { nullptr };
    detail::ReleaseWorkFn release_work_fn_ { nullptr };
    std::exception_ptr exception_ { };
};

} // namespace mcqnet::detail

namespace mcqnet::task {

template <typename T> class Task {
public:
    struct promise_type : public detail::PromiseBase<T> {
        MCQNET_NODISCARD
        inline Task get_return_object() noexcept {
            return Task { std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        template <typename TValue> inline void return_value(TValue&& value) {
            result_.emplace(std::forward<TValue>(value));
        }

        inline T take_result() {
            if ( this->exception() != nullptr ) {
                std::rethrow_exception(this->exception());
            }
            MCQNET_ASSERT(result_.has_value());
            return std::move(result_.value_ref());
        }

    private:
        detail::ResultStorage<T> result_;
    };

public:
    using handle_type = std::coroutine_handle<promise_type>;
    using value_type = T;

    Task() noexcept = default;

    explicit Task(handle_type handle) noexcept
        : handle_(handle) { }

    Task(Task&& other) noexcept
        : handle_(std::exchange(other.handle_, { })) { }

    Task& operator=(Task&& other) noexcept {
        if ( this != std::addressof(other) ) {
            reset();
            handle_ = std::exchange(other.handle_, { });
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { reset(); }

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return handle_ != nullptr; }

    MCQNET_NODISCARD
    inline bool done() const noexcept { return handle_ == nullptr || handle_.done(); }

    MCQNET_NODISCARD
    inline handle_type handle() const noexcept { return handle_; }

    MCQNET_NODISCARD
    inline handle_type release() noexcept { return std::exchange(handle_, { }); }

    inline void start() {
        MCQNET_ASSERT(handle_ != nullptr);
        if ( !handle_.done() ) {
            handle_.resume();
        }
    }

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept { return done(); }

    MCQNET_NODISCARD
    inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        MCQNET_ASSERT(handle_ != nullptr);
        auto& promise = handle_.promise();
        promise.set_continuation(continuation);
        // 若当前是在 runtime 的 run()/run_one() 里 await 子 task，则自动继承该 runtime，
        // 让父协程在子 task 完成后回到同一 ready queue。
        detail::bind_current_scheduler_if_missing(promise);
        // 当前 ready item 被消费后，若子 task 尚未完成，父协程后续还需要再回到 runtime 一次。
        promise.retain_work();
        return handle_;
    }

    inline T await_resume() {
        MCQNET_ASSERT(handle_ != nullptr);
        return handle_.promise().take_result();
    }

private:
    inline void reset() noexcept {
        if ( handle_ != nullptr ) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

private:
    handle_type handle_ { };
};

template <> class Task<void> {
public:
    struct promise_type : public detail::PromiseBase<void> {
        MCQNET_NODISCARD
        inline Task get_return_object() noexcept {
            return Task { std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        inline void return_void() noexcept { }

        inline void take_result() {
            if ( this->exception() != nullptr ) {
                std::rethrow_exception(this->exception());
            }
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    using value_type = void;

    Task() noexcept = default;

    explicit Task(handle_type handle) noexcept
        : handle_(handle) { }

    Task(Task&& other) noexcept
        : handle_(std::exchange(other.handle_, { })) { }

    Task& operator=(Task&& other) noexcept {
        if ( this != &other ) {
            reset();
            handle_ = std::exchange(other.handle_, { });
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { reset(); }

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return handle_ != nullptr; }

    MCQNET_NODISCARD
    inline bool done() const noexcept { return handle_ == nullptr || handle_.done(); }

    MCQNET_NODISCARD
    inline handle_type handle() const noexcept { return handle_; }

    MCQNET_NODISCARD
    inline handle_type release() noexcept { return std::exchange(handle_, { }); }

    inline void start() {
        MCQNET_ASSERT(handle_ != nullptr);
        if ( !handle_.done() ) {
            handle_.resume();
        }
    }

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept { return done(); }

    MCQNET_NODISCARD
    inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        MCQNET_ASSERT(handle_ != nullptr);
        auto& promise = handle_.promise();
        promise.set_continuation(continuation);
        // void 特化与有返回值版本保持同样的 runtime 继承语义。
        detail::bind_current_scheduler_if_missing(promise);
        promise.retain_work();
        return handle_;
    }

    inline void await_resume() {
        MCQNET_ASSERT(handle_ != nullptr);
        handle_.promise().take_result();
    }

private:
    inline void reset() noexcept {
        if ( handle_ != nullptr ) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

private:
    handle_type handle_ { };
};

} // namespace mcqnet::task

namespace mcqnet {

template <typename T = void> using Task = task::Task<T>;

} // namespace mcqnet
