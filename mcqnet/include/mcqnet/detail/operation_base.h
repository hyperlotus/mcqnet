#pragma once

// 协程操作状态机与调度基础抽象。

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <mcqnet/detail/macro.h>
#include <mcqnet/detail/scheduler.h>

namespace mcqnet::detail {

// 操作的生命周期状态：
// - init:      初始状态，等待被 await
// - awaiting:  已被 suspend，正在等待异步操作完成
// - completed: 正常完成，结果已就绪
// - cancelled: 被取消
enum class OperationState : std::uint8_t { init = 0, awaiting = 1, completed = 2, cancelled = 3 };

// 所有异步操作的公共基类。
// 提供统一的状态管理、协程 continuation 挂起/恢复、以及异常传播机制。
class OperationBase {
public:
    OperationBase() = default;
    OperationBase(const OperationBase&) = delete;
    OperationBase& operator=(const OperationBase&) = delete;
    virtual ~OperationBase() = default;

    // 检查操作是否已完成（正常完成或被取消）。
    MCQNET_NODISCARD
    inline bool is_completed() const noexcept {
        const OperationState current = state_.load(std::memory_order_acquire);
        return current == OperationState::completed || current == OperationState::cancelled;
    }

    // 检查操作是否已被取消。
    MCQNET_NODISCARD
    inline bool is_cancelled() const noexcept {
        return state_.load(std::memory_order_acquire) == OperationState::cancelled;
    }

    // 获取当前操作状态。
    MCQNET_NODISCARD
    inline OperationState state() const noexcept { return state_.load(std::memory_order_acquire); }

    // 设置调度器，用于将协程 continuation 调度到特定执行上下文（如 IO 线程池）。
    inline void set_scheduler(ScheduleFn schedule_fn, void* schedule_context) noexcept {
        schedule_fn_ = schedule_fn;
        schedule_context_ = schedule_context;
    }

    MCQNET_NODISCARD
    inline bool has_scheduler() const noexcept { return schedule_fn_ != nullptr; }

    inline void set_work_tracker(RetainWorkFn retain_work_fn, ReleaseWorkFn release_work_fn) noexcept {
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

    // 获取挂起在此操作上的协程句柄。
    MCQNET_NODISCARD
    inline std::coroutine_handle<> continuation() const noexcept { return continuation_; }

    // 获取操作完成时捕获的异常指针。
    MCQNET_NODISCARD
    inline std::exception_ptr completion_exception() const noexcept { return completion_exception_; }

    // 设置用户自定义数据（可用于在完成回调中传递上下文）。
    inline void set_user_data(std::uintptr_t user_data) noexcept { user_data_ = user_data; }

    MCQNET_NODISCARD
    inline std::uintptr_t user_data() const noexcept { return user_data_; }

    // 设置调试标签，便于在日志或调试器中识别操作类型。
    inline void set_debug_tag(const char* debug_tag) noexcept { debug_tag_ = debug_tag; }

    MCQNET_NODISCARD
    inline const char* debug_tag() const noexcept { return debug_tag_; }

    // 用于将操作节点链入等待队列（如 IOCP/uring 的待完成队列）。
    OperationBase* next { nullptr };

    // await_ready() 返回 false 后，框架调用此方法准备挂起。
    // 将 continuation 存入后，尝试将状态从 init 推进到 awaiting。
    // 若此时操作已完成（completed/cancelled），返回 false 通知框架立即恢复。
    MCQNET_NODISCARD
    inline bool prepare_await(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        OperationState expected = OperationState::init;
        if ( state_.compare_exchange_strong(
                 expected, OperationState::awaiting, std::memory_order_acq_rel, std::memory_order_acquire) ) {
            return true;
        }
        MCQNET_ASSERT(expected == OperationState::completed || expected == OperationState::cancelled);
        return false;
    }

    // 标记操作正常完成，提供结果码和错误码。
    // 若当前有协程在等待（awaiting），立即通过调度器恢复执行；否则仅更新状态。
    inline void complete(std::uint32_t result, std::int32_t error = 0) {
        completion_result_ = result;
        completion_error_ = error;
        completion_exception_ = nullptr;
        const OperationState previous = state_.exchange(OperationState::completed, std::memory_order_acq_rel);
        if ( previous == OperationState::awaiting ) {
            schedule_or_resume();
            return;
        }
        MCQNET_ASSERT(previous == OperationState::init || previous == OperationState::awaiting);
    }

    // 标记操作被取消。
    inline void cancel(std::int32_t error = 0) noexcept {
        completion_result_ = -1;
        completion_error_ = error;
        completion_exception_ = nullptr;
        const OperationState previous = state_.exchange(OperationState::cancelled, std::memory_order_acq_rel);
        if ( previous == OperationState::awaiting ) {
            schedule_or_resume();
            return;
        }
        MCQNET_ASSERT(previous == OperationState::init || previous == OperationState::awaiting);
    }

    // 在调用者线程同步完成时捕获异常（如同步 IO 失败）。
    inline void complete_inline_exception(std::exception_ptr exception) noexcept {
        completion_result_ = -1;
        completion_error_ = 0;
        completion_exception_ = exception;
        const OperationState previous = state_.exchange(OperationState::completed, std::memory_order_acq_rel);
        MCQNET_ASSERT(previous == OperationState::awaiting || previous == OperationState::init);
    }

protected:
    // 若存在捕获的异常，则重新抛出。通常在 await_resume() 中调用。
    inline void rethrow_if_exception() const {
        if ( completion_exception_ != nullptr ) {
            std::rethrow_exception(completion_exception_);
        }
    }

    MCQNET_NODISCARD
    inline std::int32_t completion_result() const noexcept { return completion_result_; }

    MCQNET_NODISCARD
    inline std::int32_t completion_error() const noexcept { return completion_error_; }

private:
    inline void schedule_or_resume() noexcept {
        if ( continuation_ == nullptr ) {
            return;
        }
        if ( schedule_fn_ != nullptr ) {
            schedule_fn_(schedule_context_, continuation_);
        } else {
            continuation_.resume();
        }
    }

private:
    std::atomic<OperationState> state_ { OperationState::init };
    std::coroutine_handle<> continuation_ { };
    ScheduleFn schedule_fn_ { nullptr };
    void* schedule_context_ { nullptr };
    RetainWorkFn retain_work_fn_ { nullptr };
    ReleaseWorkFn release_work_fn_ { nullptr };
    std::int32_t completion_result_ { 0 };
    std::int32_t completion_error_ { 0 };
    std::exception_ptr completion_exception_ { };
    std::uintptr_t user_data_ { 0 };
    const char* debug_tag_ { nullptr };
};

} // namespace mcqnet::detail
