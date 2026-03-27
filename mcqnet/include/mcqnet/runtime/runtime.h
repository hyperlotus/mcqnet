#pragma once

// Runtime 层公共 API。
// 当前先提供一个最小单线程 ready queue runtime：
// - `post()` 把 continuation 放进就绪队列
// - `run_one()` 非阻塞地执行一个就绪任务
// - `run()` 阻塞等待任务，直到 `stop()` 后把队列 drain 完
//
// 更复杂的 timer / IO backend / 多线程调度仍留到后续阶段。

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mcqnet/config/assert.h>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/macro.h>
#include <mcqnet/detail/scheduler.h>
#include <mcqnet/task/spawn.h>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

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

    explicit Handle(Runtime* runtime) noexcept
        : runtime_(runtime) { }

    Runtime* runtime_ { nullptr };
};

// Runtime 是用户可见的顶层执行上下文。
// 当前实现先提供最小单线程 ready queue event loop，为后续 IO backend 留统一入口。
class Runtime {
public:
    Runtime() noexcept = default;
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
    inline bool stopped() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

    // 向 runtime 提交一个待恢复的协程。
    // 这条 public 入口只接收新的外部提交；stop() 之后会拒绝新任务。
    // 该操作是线程安全的，可由外部线程唤醒正在 run() 中等待的 runtime。
    // 这里会新增一份 pending work，直到该 continuation 最终完成或把恢复义务转移给别的 await 点。
    inline void post(std::coroutine_handle<> continuation) {
        if ( continuation == nullptr ) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( stopped_ ) {
                core::throw_runtime_error(
                    core::error_code { core::errc::runtime_stopped }, "Runtime::post() called after stop()");
            }
            enqueue_new_work_unlocked(continuation, false);
        }
        ready_cv_.notify_one();
    }

    // 运行 event loop。
    // 当前实现会阻塞等待 ready queue 中出现工作；
    // stop() 之后不再接收新的 public work，但 run() 只有在以下条件同时满足时才会返回：
    // - stop() 已被请求
    // - ready queue 已空
    // - pending work 也为 0（即没有还会回来的挂起续体）
    // 该入口带单线程驱动保护：若 runtime 已经在 run()/run_one() 中被驱动，会抛出 invalid_state。
    inline void run() {
        DriverScope driver_scope(this, DriveEntry::run);
        for ( ;; ) {
            ReadyItem ready_item { };
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_cv_.wait(lock, [this] { return !ready_queue_.empty() || (stopped_ && pending_work_ == 0); });
                if ( ready_queue_.empty() ) {
                    MCQNET_ASSERT(stopped_);
                    MCQNET_ASSERT(pending_work_ == 0);
                    return;
                }
                ready_item = ready_queue_.front();
                ready_queue_.pop_front();
            }

            if ( ready_item.continuation != nullptr ) {
                // 在实际 resume 前安装当前 runtime 的调度作用域。
                // 这样该 continuation 内部再去 await Task / JoinHandle / Operation 时，
                // 它们会默认把后续恢复路径绑回这个 runtime。
                detail::SchedulerScope scheduler_scope(
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
    // 与 run() 一样，若 runtime 已经被其他 run()/run_one() 调用驱动，会抛出 invalid_state。
    inline bool run_one() {
        DriverScope driver_scope(this, DriveEntry::run_one);
        ReadyItem ready_item { };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( ready_queue_.empty() ) {
                return false;
            }
            ready_item = ready_queue_.front();
            ready_queue_.pop_front();
        }

        if ( ready_item.continuation != nullptr ) {
            // run_one() 与 run() 保持同样的调度上下文语义。
            detail::SchedulerScope scheduler_scope(
                &Runtime::schedule_ready_continuation, this, &Runtime::retain_pending_work, &Runtime::release_pending_work);
            ready_item.continuation.resume();
        }
        finish_ready_item(ready_item);
        return true;
    }

    // 请求 runtime 停止接受新的 public 提交，并唤醒 run()。
    // 已经启动或已入队的 continuation 仍允许被 drain 完。
    // 该操作可从 runtime 外部线程调用。
    inline void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        ready_cv_.notify_all();
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

        auto state = std::make_shared<detail::JoinState<T>>();
        auto join_handle = detail::JoinHandleAccess<T>::make(state);
        join_handle.set_scheduler(&Runtime::schedule_ready_continuation, this);

        auto bridge = detail::bridge_task_to_join_handle_queued(std::move(task_value), std::move(state));
        MCQNET_ASSERT(bridge.valid());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ( stopped_ ) {
                core::throw_runtime_error(
                    core::error_code { core::errc::runtime_stopped }, "Runtime::spawn() called after stop()");
            }
            enqueue_new_work_unlocked(bridge.release(), true);
        }
        ready_cv_.notify_one();
        return join_handle;
    }

private:
    enum class DriveEntry : std::uint8_t { run = 0, run_one = 1 };

    struct ReadyItem {
        std::coroutine_handle<> continuation { };
        bool owns_handle { false };
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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_queue_.push_back(ReadyItem { continuation, false });
        }
        ready_cv_.notify_one();
    }

    inline void enqueue_new_work_unlocked(std::coroutine_handle<> continuation, bool owns_handle) {
        ready_queue_.push_back(ReadyItem { continuation, owns_handle });
        if ( owns_handle && continuation != nullptr ) {
            owned_continuations_.insert(continuation.address());
        }
        ++pending_work_;
    }

    inline void retain_pending_work() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ++pending_work_;
    }

    inline void release_pending_work() noexcept {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(pending_work_ > 0);
            if ( pending_work_ > 0 ) {
                --pending_work_;
            }
            notify = stopped_ && pending_work_ == 0;
        }
        if ( notify ) {
            ready_cv_.notify_all();
        }
    }

    inline void finish_ready_item(const ReadyItem& ready_item) noexcept {
        bool destroy_owned_handle = false;
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            MCQNET_ASSERT(pending_work_ > 0);
            if ( pending_work_ > 0 ) {
                --pending_work_;
            }
            destroy_owned_handle = ready_item.continuation != nullptr && ready_item.continuation.done()
                && owned_continuations_.erase(ready_item.continuation.address()) > 0;
            notify = stopped_ && pending_work_ == 0;
        }

        if ( destroy_owned_handle ) {
            ready_item.continuation.destroy();
        }
        if ( notify ) {
            ready_cv_.notify_all();
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
    std::unordered_set<void*> owned_continuations_;
    std::size_t pending_work_ { 0 };
    bool stopped_ { false };
    bool driving_ { false };
    std::thread::id driver_thread_id_ { };
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
