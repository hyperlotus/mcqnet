#pragma once

// 内部调度上下文工具。
// 用线程局部状态表达“当前协程正在某个 scheduler / runtime 上执行”，
// 供 Task / JoinHandle / OperationBase 在 await 时补齐 continuation 恢复路径。

#include <coroutine>
#include <mcqnet/detail/macro.h>

namespace mcqnet::detail {

// 调度函数类型，用于将协程 continuation 调度到特定执行上下文。
using ScheduleFn = void (*)(void* context, std::coroutine_handle<> handle) noexcept;

// work tracker 回调用于告诉调度器：
// - 一个将来还会回来的 continuation 已经挂起
// - 一个挂起义务已经被同步消解，不再需要等待
using RetainWorkFn = void (*)(void* context) noexcept;
using ReleaseWorkFn = void (*)(void* context) noexcept;

// 一个轻量绑定对，描述“把 continuation 投递到哪里去”。
// 当前主要由 runtime 在 run()/run_one() 恢复协程时临时安装。
struct SchedulerBinding {
    ScheduleFn schedule_fn { nullptr };
    void* schedule_context { nullptr };
    RetainWorkFn retain_work_fn { nullptr };
    ReleaseWorkFn release_work_fn { nullptr };

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return schedule_fn != nullptr; }

    MCQNET_NODISCARD
    inline bool tracks_work() const noexcept { return retain_work_fn != nullptr; }
};

inline thread_local SchedulerBinding tls_scheduler_binding { };

// SchedulerScope 在当前线程安装一个临时调度上下文。
// runtime 恢复 continuation 时会构造它；scope 析构后恢复先前上下文。
class SchedulerScope {
public:
    SchedulerScope(
        ScheduleFn schedule_fn,
        void* schedule_context,
        RetainWorkFn retain_work_fn = nullptr,
        ReleaseWorkFn release_work_fn = nullptr) noexcept
        : previous_(tls_scheduler_binding) {
        tls_scheduler_binding = SchedulerBinding { schedule_fn, schedule_context, retain_work_fn, release_work_fn };
    }

    SchedulerScope(const SchedulerScope&) = delete;
    SchedulerScope& operator=(const SchedulerScope&) = delete;

    ~SchedulerScope() { tls_scheduler_binding = previous_; }

    MCQNET_NODISCARD
    static inline SchedulerBinding current() noexcept { return tls_scheduler_binding; }

private:
    SchedulerBinding previous_ { };
};

// 若当前线程已经处于某个 runtime / scheduler 上，就把该恢复策略补给目标对象。
// 这让 Task / JoinHandle / OperationBase 在 runtime 内被 await 时，默认继续回到同一 runtime。
template <typename TSchedulerAware> inline void bind_current_scheduler_if_missing(TSchedulerAware& value) noexcept {
    const SchedulerBinding scheduler = SchedulerScope::current();

    if constexpr ( requires(TSchedulerAware& candidate) {
                       candidate.has_scheduler();
                       candidate.set_scheduler(static_cast<ScheduleFn>(nullptr), static_cast<void*>(nullptr));
                   } ) {
        if ( !value.has_scheduler() && scheduler.valid() ) {
            value.set_scheduler(scheduler.schedule_fn, scheduler.schedule_context);
        }
    }

    if constexpr ( requires(TSchedulerAware& candidate) {
                       candidate.has_work_tracker();
                       candidate.set_work_tracker(static_cast<RetainWorkFn>(nullptr), static_cast<ReleaseWorkFn>(nullptr));
                   } ) {
        if ( !value.has_work_tracker() && scheduler.tracks_work() ) {
            value.set_work_tracker(scheduler.retain_work_fn, scheduler.release_work_fn);
        }
    }
}

} // namespace mcqnet::detail
