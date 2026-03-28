#pragma once

// 协程 awaiter 适配器，将 OperationBase 系列操作接入 C++ 协程框架。

#include <coroutine>
#include <exception>
#include <mcqnet/detail/macro.h>
#include <mcqnet/detail/operation_base.h>

namespace mcqnet::detail {

// OperationAwaiter<T> 将符合 OperationBase 接口的操作类型适配为可 co_await 的 awaiter。
// 协同工作流程：
// 1. await_ready()   -> 判断操作是否已完成，若已完成则跳过 suspend
// 2. await_suspend() -> 将 continuation 注册到操作，准备提交异步请求
// 3. await_resume()  -> 返回操作结果或重新抛出捕获的异常
template <typename TOperation> class OperationAwaiter {
public:
    explicit OperationAwaiter(TOperation& operation) noexcept
        : operation_(operation) { }

    OperationAwaiter(const OperationAwaiter&) = delete;
    OperationAwaiter& operator=(const OperationAwaiter&) = delete;

    MCQNET_NODISCARD
    inline bool await_ready() const noexcept { return operation_.is_completed(); }

    inline bool await_suspend(std::coroutine_handle<> continuation) {
        if constexpr ( requires(TOperation& operation) { operation.bind_explicit_runtime_if_missing(); } ) {
            operation_.bind_explicit_runtime_if_missing();
        }
        // 若操作对象尚未指定 scheduler，则继承当前 runtime。
        // 这样 operation.complete() 触发恢复时会把 continuation 放回 runtime ready queue。
        bind_current_scheduler_if_missing(operation_);
        if ( !operation_.prepare_await(continuation) ) {
            return false;
        }
        // 当前 ready item 在返回后会被消费；这里先为“操作完成后还要再恢复一次”保留一份 pending work。
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

    decltype(auto) await_resume() { return operation_.await_resume(); }

private:
    TOperation& operation_;
};

template <typename TOperation>
MCQNET_NODISCARD inline OperationAwaiter<TOperation> make_operation_awaiter(TOperation& operation) noexcept {
    return OperationAwaiter<TOperation> { operation };
}
} // namespace mcqnet::detail
