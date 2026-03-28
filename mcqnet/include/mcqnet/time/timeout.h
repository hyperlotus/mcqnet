#pragma once

// timeout 基元。
// 当前阶段它是一个“deadline 触发则抛 timed_out”的 awaitable；
// 后续 select/race 组合子可以直接把它当作 timeout 分支使用。

#include <mcqnet/time/sleep.h>

namespace mcqnet::time {

template <typename TDuration>
MCQNET_NODISCARD inline auto timeout(TDuration duration, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::timeout> {
        runtime::Handle { }, detail::deadline_after(duration), std::move(cancel_token)
    };
}

template <typename TDuration>
MCQNET_NODISCARD inline auto timeout(runtime::Handle runtime_handle, TDuration duration, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::timeout> {
        runtime_handle, detail::deadline_after(duration), std::move(cancel_token)
    };
}

MCQNET_NODISCARD inline auto timeout(
    runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::timeout> {
        runtime::Handle { }, deadline, std::move(cancel_token)
    };
}

MCQNET_NODISCARD inline auto timeout(
    runtime::Handle runtime_handle, runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {}) {
    return detail::BasicTimerAwaitable<detail::TimerMode::timeout> {
        runtime_handle, deadline, std::move(cancel_token)
    };
}

} // namespace mcqnet::time

namespace mcqnet {

using time::timeout;

} // namespace mcqnet
