#pragma once

// 协程任务层公共接口聚合头。
// 当前自由函数 `spawn()` 仍是“直接桥接 Task -> JoinHandle”的过渡方案。
// 后续 runtime 落地后，`runtime::Runtime::spawn()` 会成为更完整的执行上下文入口，
// 但这里的 task 级 API 仍保留其轻量桥接语义。

#include <exception>
#include <mcqnet/task/join_handle.h>
#include <mcqnet/task/task.h>
#include <type_traits>
#include <utility>

namespace mcqnet::detail {

class DetachedTask {
public:
    struct promise_type {
        MCQNET_NODISCARD
        inline DetachedTask get_return_object() noexcept { return { }; }

        MCQNET_NODISCARD
        inline std::suspend_never initial_suspend() const noexcept { return { }; }

        MCQNET_NODISCARD
        inline std::suspend_never final_suspend() const noexcept { return { }; }

        inline void return_void() const noexcept { }

        inline void unhandled_exception() const noexcept { std::terminate(); }
    };
};

template <typename T>
DetachedTask bridge_task_to_join_handle(::mcqnet::task::Task<T> task, std::shared_ptr<JoinState<T>> state) {
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

namespace mcqnet::task {

// 把 Task<T> 桥接成 JoinHandle<T>。
// 当前实现会在调用线程立即启动内部桥接协程，而不是提交给 runtime 调度。
template <typename T> MCQNET_NODISCARD inline JoinHandle<T> spawn(Task<T> task) {
    MCQNET_ASSERT(task.valid());
    if ( !task.valid() ) {
        return { };
    }

    auto state = std::make_shared<detail::JoinState<T>>();
    auto join_handle = detail::JoinHandleAccess<T>::make(state);
    detail::bridge_task_to_join_handle(std::move(task), std::move(state));
    return join_handle;
}

} // namespace mcqnet::task

namespace mcqnet {

using task::spawn;

} // namespace mcqnet
