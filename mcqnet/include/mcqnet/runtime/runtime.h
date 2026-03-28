#pragma once

// Runtime 层公共 API。
// 当前先提供一个最小单线程 ready queue runtime：
// - `post()` 把 continuation 放进就绪队列
// - `run_one()` 非阻塞地执行一个就绪任务
// - `run()` 阻塞等待任务，直到 `stop()` 后把队列 drain 完
//
// 更复杂的 timer / IO backend / 多线程调度仍留到后续阶段。

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mcqnet/config/assert.h>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/macro.h>
#include <mcqnet/detail/scheduler.h>
#include <mcqnet/runtime/completion_backend.h>
#include <mcqnet/task/spawn.h>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcqnet::detail {

// runtime::Runtime::spawn() 使用的内部桥接协程。
// 它在 initial_suspend 时先挂起，等 runtime 把首个 resume 放进 ready queue。
class QueuedBridgeTask {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        MCQNET_NODISCARD
        inline QueuedBridgeTask get_return_object() noexcept {
            return QueuedBridgeTask { handle_type::from_promise(*this) };
        }

        MCQNET_NODISCARD
        inline std::suspend_always initial_suspend() const noexcept { return { }; }

        MCQNET_NODISCARD
        // runtime 拥有该桥接协程句柄，因此 final_suspend 先停住，
        // 让 runtime 在确认 done() 后显式 destroy()。
        inline std::suspend_always final_suspend() const noexcept { return { }; }

        inline void return_void() const noexcept { }

        inline void unhandled_exception() const noexcept { std::terminate(); }
    };

    QueuedBridgeTask() noexcept = default;

    explicit QueuedBridgeTask(handle_type handle) noexcept
        : handle_(handle) { }

    QueuedBridgeTask(QueuedBridgeTask&& other) noexcept
        : handle_(std::exchange(other.handle_, { })) { }

    QueuedBridgeTask& operator=(QueuedBridgeTask&& other) noexcept {
        if ( this != std::addressof(other) ) {
            reset();
            handle_ = std::exchange(other.handle_, { });
        }
        return *this;
    }

    QueuedBridgeTask(const QueuedBridgeTask&) = delete;
    QueuedBridgeTask& operator=(const QueuedBridgeTask&) = delete;

    ~QueuedBridgeTask() { reset(); }

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return handle_ != nullptr; }

    MCQNET_NODISCARD
    inline handle_type release() noexcept { return std::exchange(handle_, { }); }

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

template <typename T>
QueuedBridgeTask bridge_task_to_join_handle_queued(
    ::mcqnet::task::Task<T> task, std::shared_ptr<JoinState<T>> state) {
    try {
        if constexpr ( std::is_void_v<T> ) {
            co_await std::move(task);
            state->set_value();
        } else {
            state->set_value(co_await std::move(task));
        }
    } catch ( ... ) {
        state->set_exception(std::current_exception());
    }
}

} // namespace mcqnet::detail

namespace mcqnet::time::detail {

struct RuntimeTimerAccess;

} // namespace mcqnet::time::detail

namespace mcqnet::runtime::detail {

struct RuntimeIoAccess;

} // namespace mcqnet::runtime::detail

namespace mcqnet::runtime {

class Runtime;

// Handle 是 Runtime 的轻量引用视图。
// 它不拥有 runtime 生命周期，只负责把投递和 spawn 请求转发给关联 runtime。
class Handle {
public:
    Handle() noexcept = default;

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return runtime_ != nullptr; }

    MCQNET_NODISCARD
    inline explicit operator bool() const noexcept { return valid(); }

    // 将 continuation 投递到关联 runtime。
    // public post 只接受用户新的提交；若 runtime 已 stop，会抛出 runtime_stopped。
    inline void post(std::coroutine_handle<> continuation) const;

    // 通过关联 runtime 启动任务。
    // 当前实现会把桥接协程首个 resume 放进 runtime 的 ready queue。
    template <typename T> MCQNET_NODISCARD inline task::JoinHandle<T> spawn(task::Task<T> task_value) const;

private:
    friend class Runtime;
    friend struct ::mcqnet::time::detail::RuntimeTimerAccess;
    friend struct ::mcqnet::runtime::detail::RuntimeIoAccess;

    explicit Handle(Runtime* runtime) noexcept
        : runtime_(runtime) { }

    Runtime* runtime_ { nullptr };
};

// Runtime 是用户可见的顶层执行上下文。
// 当前实现先提供最小单线程 ready queue event loop，为后续 IO backend 留统一入口。
class Runtime {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    explicit Runtime(CompletionBackend* completion_backend = nullptr) noexcept
        : completion_backend_(completion_backend) { }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    ~Runtime() {
        std::lock_guard<std::mutex> lock(mutex_);
        for ( void* continuation_address : owned_continuations_ ) {
            std::coroutine_handle<>::from_address(continuation_address).destroy();
        }
    }

    MCQNET_NODISCARD
    inline Handle handle() noexcept { return Handle { this }; }

    MCQNET_NODISCARD
    static inline Runtime* current() noexcept { return tls_current_runtime_; }

    MCQNET_NODISCARD
    static inline Handle current_handle() noexcept { return Handle { tls_current_runtime_ }; }

    MCQNET_NODISCARD
    inline bool stopped() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

    // backend 由调用方持有生命周期；runtime 只保存一个非 owning 指针。
    // 为了避免在已有 pending work/timer 时切换 backend，这里只允许在 idle 状态配置。
    inline void set_completion_backend(CompletionBackend* completion_backend) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( driving_ || pending_work_ != 0 || !ready_queue_.empty() || !timers_.empty() ) {
            core::throw_runtime_error(
                core::error_code { core::errc::invalid_state },
                "Runtime::set_completion_backend() requires an idle runtime");
        }
        completion_backend_ = completion_backend;
    }

    MCQNET_NODISCARD
    inline CompletionBackend* completion_backend() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return completion_backend_;
    }

    // 向 runtime 提交一个待恢复的协程。
    // 这条 public 入口只接收新的外部提交；stop() 之后会拒绝新任务。
    // 该操作是线程安全的，可由外部线程唤醒正在 run() 中等待的 runtime。
    // 这里会新增一份 pending work，直到该 continuation 最终完成或把恢复义务转移给别的 await 点。
    //
    // pending work 的正式覆盖范围：
    // - public `post()` / `spawn()` 提交进来的新 continuation
    // - 以及显式接入 `SchedulerBinding` 协议的 awaitable
    //   （即：await_suspend 前 retain_work()，异步完成时经 schedule_fn 回到 runtime）
    // 未接入这套协议的第三方 awaitable，不会自动计入 pending work。
    inline void post(std::coroutine_handle<> continuation) {
        if ( continuation == nullptr ) {
            return;
        }

        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( stopped_ ) {
                core::throw_runtime_error(
                    core::error_code { core::errc::runtime_stopped }, "Runtime::post() called after stop()");
            }
            enqueue_new_work_unlocked(continuation, false);
            completion_backend = completion_backend_;
        }
        ready_cv_.notify_one();
        if ( completion_backend != nullptr ) {
            completion_backend->wake();
        }
    }

    // 运行 event loop。
    // 当前实现会阻塞等待 ready queue 中出现工作；
    // stop() 之后不再接收新的 public work，但 run() 只有在以下条件同时满足时才会返回：
    // - stop() 已被请求
    // - ready queue 已空
    // - pending work 也为 0（即没有还会回来的挂起续体）
    // 若 runtime 配置了 completion backend，则在 ready queue 为空且存在 pending work 时，
    // run() 会把等待时间委托给 backend::poll(timeout)。
    // 该入口带单线程驱动保护：若 runtime 已经在 run()/run_one() 中被驱动，会抛出 invalid_state。
    inline void run() {
        DriverScope driver_scope(this, DriveEntry::run);
        for ( ;; ) {
            ReadyItem ready_item { };
            std::vector<TimerDispatch> expired_timers;
            CompletionBackend* completion_backend = nullptr;
            clock::duration backend_timeout = clock::duration::zero();
            {
                std::unique_lock<std::mutex> lock(mutex_);
                collect_expired_timers_unlocked(clock::now(), expired_timers);
                if ( expired_timers.empty() ) {
                    if ( !ready_queue_.empty() ) {
                        ready_item = ready_queue_.front();
                        ready_queue_.pop_front();
                    } else if ( stopped_ && pending_work_ == 0 ) {
                        return;
                    } else if ( completion_backend_ != nullptr && pending_work_ > 0 ) {
                        completion_backend = completion_backend_;
                        backend_timeout = compute_backend_timeout_unlocked(clock::now());
                    } else {
                        const std::optional<time_point> next_deadline = next_timer_deadline_unlocked();
                        if ( next_deadline.has_value() ) {
                            ready_cv_.wait_until(
                                lock,
                                *next_deadline,
                                [this] { return !ready_queue_.empty() || (stopped_ && pending_work_ == 0); });
                        } else {
                            ready_cv_.wait(lock, [this] { return !ready_queue_.empty() || (stopped_ && pending_work_ == 0); });
                        }
                        continue;
                    }
                }
            }

            invoke_timer_dispatches(expired_timers);
            if ( !expired_timers.empty() ) {
                continue;
            }
            if ( completion_backend != nullptr ) {
                completion_backend->poll(backend_timeout);
                continue;
            }
            if ( ready_item.continuation != nullptr ) {
                // 在实际 resume 前安装当前 runtime 的调度作用域。
                // 这样该 continuation 内部再去 await Task / JoinHandle / Operation 时，
                // 它们会默认把后续恢复路径绑回这个 runtime。
                CurrentRuntimeScope current_runtime_scope(this);
                ::mcqnet::detail::SchedulerScope scheduler_scope(
                    &Runtime::schedule_ready_continuation, this, &Runtime::retain_pending_work, &Runtime::release_pending_work);
                ready_item.continuation.resume();
            }
            finish_ready_item(ready_item);
        }
    }

    MCQNET_NODISCARD
    // 运行一次调度循环。
    // 这是非阻塞入口：若当前没有 ready work，直接返回 false。
    // 即使 pending work 仍大于 0，只要当前没有 ready continuation，它也不会阻塞等待。
    // 若配置了 completion backend，则会额外执行一次 `poll(0)` 尝试吃掉已完成的 IO。
    // 与 run() 一样，若 runtime 已经被其他 run()/run_one() 调用驱动，会抛出 invalid_state。
    inline bool run_one() {
        DriverScope driver_scope(this, DriveEntry::run_one);
        bool backend_polled = false;
        for ( ;; ) {
            ReadyItem ready_item { };
            std::vector<TimerDispatch> expired_timers;
            CompletionBackend* completion_backend = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                collect_expired_timers_unlocked(clock::now(), expired_timers);
                if ( !ready_queue_.empty() ) {
                    ready_item = ready_queue_.front();
                    ready_queue_.pop_front();
                } else if ( !backend_polled && completion_backend_ != nullptr && pending_work_ > 0 ) {
                    completion_backend = completion_backend_;
                } else {
                    return false;
                }
            }

            invoke_timer_dispatches(expired_timers);
            if ( !expired_timers.empty() ) {
                continue;
            }
            if ( completion_backend != nullptr ) {
                backend_polled = true;
                completion_backend->poll(clock::duration::zero());
                continue;
            }

            if ( ready_item.continuation != nullptr ) {
                // run_one() 与 run() 保持同样的调度上下文语义。
                CurrentRuntimeScope current_runtime_scope(this);
                ::mcqnet::detail::SchedulerScope scheduler_scope(
                    &Runtime::schedule_ready_continuation, this, &Runtime::retain_pending_work, &Runtime::release_pending_work);
                ready_item.continuation.resume();
            }
            finish_ready_item(ready_item);
            return true;
        }
    }

    // 请求 runtime 停止接受新的 public 提交，并唤醒 run()。
    // 已经启动或已入队的 continuation 仍允许被 drain 完。
    // 该操作可从 runtime 外部线程调用。
    inline void stop() noexcept {
        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            completion_backend = completion_backend_;
        }
        ready_cv_.notify_all();
        if ( completion_backend != nullptr ) {
            completion_backend->wake();
        }
    }

    // 通过 runtime 启动任务并返回 JoinHandle。
    // 与自由函数 task::spawn() 的区别是：
    // - bridge 协程首个 resume 走 runtime ready queue
    // - 返回的 JoinHandle continuation 也会回到这个 runtime
    template <typename T> MCQNET_NODISCARD inline task::JoinHandle<T> spawn(task::Task<T> task_value) {
        MCQNET_ASSERT(task_value.valid());
        if ( !task_value.valid() ) {
            return { };
        }

        auto state = std::make_shared<::mcqnet::detail::JoinState<T>>();
        auto join_handle = ::mcqnet::detail::JoinHandleAccess<T>::make(state);
        join_handle.set_scheduler(&Runtime::schedule_ready_continuation, this);

        auto bridge = ::mcqnet::detail::bridge_task_to_join_handle_queued(std::move(task_value), std::move(state));
        MCQNET_ASSERT(bridge.valid());

        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( stopped_ ) {
                core::throw_runtime_error(
                    core::error_code { core::errc::runtime_stopped }, "Runtime::spawn() called after stop()");
            }
            enqueue_new_work_unlocked(bridge.release(), true);
            completion_backend = completion_backend_;
        }
        ready_cv_.notify_one();
        if ( completion_backend != nullptr ) {
            completion_backend->wake();
        }
        return join_handle;
    }

private:
    friend struct ::mcqnet::time::detail::RuntimeTimerAccess;
    friend struct ::mcqnet::runtime::detail::RuntimeIoAccess;

    enum class DriveEntry : std::uint8_t { run = 0, run_one = 1 };

    struct ReadyItem {
        std::coroutine_handle<> continuation { };
        bool owns_handle { false };
    };

    using TimerCallbackFn = void (*)(void*) noexcept;

    struct TimerDispatch {
        TimerCallbackFn callback { nullptr };
        void* context { nullptr };
    };

    struct TimerEntry {
        time_point deadline { };
        TimerCallbackFn callback { nullptr };
        void* context { nullptr };
        std::multimap<time_point, std::uint64_t>::iterator deadline_iterator { };
    };

    class DriverScope {
    public:
        DriverScope(Runtime* runtime, DriveEntry entry)
            : runtime_(runtime) {
            runtime_->begin_drive(entry);
        }

        DriverScope(const DriverScope&) = delete;
        DriverScope& operator=(const DriverScope&) = delete;

        ~DriverScope() {
            if ( runtime_ != nullptr ) {
                runtime_->end_drive();
            }
        }

    private:
        Runtime* runtime_ { nullptr };
    };

    class CurrentRuntimeScope {
    public:
        explicit CurrentRuntimeScope(Runtime* runtime) noexcept
            : previous_(tls_current_runtime_) {
            tls_current_runtime_ = runtime;
        }

        CurrentRuntimeScope(const CurrentRuntimeScope&) = delete;
        CurrentRuntimeScope& operator=(const CurrentRuntimeScope&) = delete;

        ~CurrentRuntimeScope() { tls_current_runtime_ = previous_; }

    private:
        Runtime* previous_ { nullptr };
    };

    static inline void schedule_ready_continuation(void* schedule_context, std::coroutine_handle<> continuation)
        noexcept {
        MCQNET_ASSERT(schedule_context != nullptr);
        static_cast<Runtime*>(schedule_context)->enqueue_ready_continuation(continuation);
    }

    static inline void retain_pending_work(void* schedule_context) noexcept {
        MCQNET_ASSERT(schedule_context != nullptr);
        static_cast<Runtime*>(schedule_context)->retain_pending_work();
    }

    static inline void release_pending_work(void* schedule_context) noexcept {
        MCQNET_ASSERT(schedule_context != nullptr);
        static_cast<Runtime*>(schedule_context)->release_pending_work();
    }

    // 内部调度入口。
    // 与 public post() 不同，这里允许 stop() 之后继续入队，确保已经挂起的续体仍能被 drain。
    // 该路径同样支持外部线程在异步完成时把 continuation 投递回 runtime。
    inline void enqueue_ready_continuation(std::coroutine_handle<> continuation) noexcept {
        if ( continuation == nullptr ) {
            return;
        }

        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_queue_.push_back(ReadyItem { continuation, false });
            completion_backend = completion_backend_;
        }
        ready_cv_.notify_one();
        if ( completion_backend != nullptr ) {
            completion_backend->wake();
        }
    }

    inline void enqueue_new_work_unlocked(std::coroutine_handle<> continuation, bool owns_handle) {
        ready_queue_.push_back(ReadyItem { continuation, owns_handle });
        if ( owns_handle && continuation != nullptr ) {
            owned_continuations_.insert(continuation.address());
        }
        ++pending_work_;
    }

    MCQNET_NODISCARD
    inline std::uint64_t schedule_timer(TimerCallbackFn callback, void* context, time_point deadline) {
        MCQNET_ASSERT(callback != nullptr);

        std::uint64_t timer_id = 0;
        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool wake_driver = timers_by_deadline_.empty() || deadline < timers_by_deadline_.begin()->first;
            timer_id = next_timer_id_++;
            const auto deadline_iterator = timers_by_deadline_.emplace(deadline, timer_id);
            timers_.emplace(timer_id, TimerEntry { deadline, callback, context, deadline_iterator });
            completion_backend = wake_driver ? completion_backend_ : nullptr;
        }

        ready_cv_.notify_all();
        if ( completion_backend != nullptr ) {
            completion_backend->wake();
        }
        return timer_id;
    }

    inline bool cancel_timer(std::uint64_t timer_id) noexcept {
        bool cancelled = false;
        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto entry = timers_.find(timer_id);
            if ( entry == timers_.end() ) {
                return false;
            }
            timers_by_deadline_.erase(entry->second.deadline_iterator);
            timers_.erase(entry);
            cancelled = true;
            completion_backend = completion_backend_;
        }

        ready_cv_.notify_all();
        if ( completion_backend != nullptr ) {
            completion_backend->wake();
        }
        return cancelled;
    }

    inline void collect_expired_timers_unlocked(time_point now, std::vector<TimerDispatch>& expired_timers) {
        while ( !timers_by_deadline_.empty() && timers_by_deadline_.begin()->first <= now ) {
            const auto deadline_entry = timers_by_deadline_.begin();
            const std::uint64_t timer_id = deadline_entry->second;
            const auto timer_entry = timers_.find(timer_id);
            if ( timer_entry != timers_.end() ) {
                expired_timers.push_back(TimerDispatch { timer_entry->second.callback, timer_entry->second.context });
                timers_.erase(timer_entry);
            }
            timers_by_deadline_.erase(deadline_entry);
        }
    }

    inline void invoke_timer_dispatches(const std::vector<TimerDispatch>& expired_timers) noexcept {
        for ( const TimerDispatch& dispatch : expired_timers ) {
            if ( dispatch.callback != nullptr ) {
                dispatch.callback(dispatch.context);
            }
        }
    }

    MCQNET_NODISCARD
    inline std::optional<time_point> next_timer_deadline_unlocked() const noexcept {
        if ( timers_by_deadline_.empty() ) {
            return std::nullopt;
        }
        return timers_by_deadline_.begin()->first;
    }

    MCQNET_NODISCARD
    inline clock::duration compute_backend_timeout_unlocked(time_point now) const noexcept {
        const std::optional<time_point> next_deadline = next_timer_deadline_unlocked();
        if ( !next_deadline.has_value() ) {
            return clock::duration::max();
        }
        if ( *next_deadline <= now ) {
            return clock::duration::zero();
        }
        return *next_deadline - now;
    }

    inline void retain_pending_work() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ++pending_work_;
    }

    inline void release_pending_work() noexcept {
        bool notify = false;
        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(pending_work_ > 0);
            if ( pending_work_ > 0 ) {
                --pending_work_;
            }
            notify = stopped_ && pending_work_ == 0;
            completion_backend = notify ? completion_backend_ : nullptr;
        }
        if ( notify ) {
            ready_cv_.notify_all();
            if ( completion_backend != nullptr ) {
                completion_backend->wake();
            }
        }
    }

    inline void finish_ready_item(const ReadyItem& ready_item) noexcept {
        bool destroy_owned_handle = false;
        bool notify = false;
        CompletionBackend* completion_backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(pending_work_ > 0);
            if ( pending_work_ > 0 ) {
                --pending_work_;
            }
            destroy_owned_handle = ready_item.continuation != nullptr && ready_item.continuation.done()
                && owned_continuations_.erase(ready_item.continuation.address()) > 0;
            notify = stopped_ && pending_work_ == 0;
            completion_backend = notify ? completion_backend_ : nullptr;
        }

        if ( destroy_owned_handle ) {
            ready_item.continuation.destroy();
        }
        if ( notify ) {
            ready_cv_.notify_all();
            if ( completion_backend != nullptr ) {
                completion_backend->wake();
            }
        }
    }

    // run()/run_one() 在同一时刻只允许一个驱动者进入。
    // 这既防止多个线程并发 drive 同一个 runtime，也防止在 continuation 内部重入 drive。
    inline void begin_drive(DriveEntry entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ( driving_ ) {
            core::throw_runtime_error(
                core::error_code { core::errc::invalid_state },
                entry == DriveEntry::run ? "Runtime::run() called while the runtime is already being driven"
                                         : "Runtime::run_one() called while the runtime is already being driven");
        }
        driving_ = true;
        driver_thread_id_ = std::this_thread::get_id();
    }

    inline void end_drive() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        MCQNET_ASSERT(driving_);
        driving_ = false;
        driver_thread_id_ = std::thread::id { };
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::deque<ReadyItem> ready_queue_;
    std::multimap<time_point, std::uint64_t> timers_by_deadline_;
    std::unordered_map<std::uint64_t, TimerEntry> timers_;
    std::unordered_set<void*> owned_continuations_;
    std::uint64_t next_timer_id_ { 1 };
    CompletionBackend* completion_backend_ { nullptr };
    std::size_t pending_work_ { 0 };
    bool stopped_ { false };
    bool driving_ { false };
    std::thread::id driver_thread_id_ { };
    inline static thread_local Runtime* tls_current_runtime_ = nullptr;
};

inline void Handle::post(std::coroutine_handle<> continuation) const {
    MCQNET_ASSERT(runtime_ != nullptr);
    runtime_->post(continuation);
}

template <typename T> inline task::JoinHandle<T> Handle::spawn(task::Task<T> task_value) const {
    MCQNET_ASSERT(runtime_ != nullptr);
    return runtime_->spawn(std::move(task_value));
}

} // namespace mcqnet::runtime
