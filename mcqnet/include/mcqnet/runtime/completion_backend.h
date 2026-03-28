#pragma once

// runtime 与真实 IO completion backend 的最小对接点。
// 设计约束：
// - runtime 只负责在 idle 时调用 poll()，以及在本地 ready/timer/stop 事件出现时调用 wake()
// - backend 自己管理平台完成队列，并在拿到完成事件后调用 OperationBase::complete()/cancel()
// - operation 的 continuation 如何回到 runtime，由 await 时注入的 scheduler/work-tracker 决定

#include <chrono>
#include <mcqnet/detail/macro.h>

namespace mcqnet::runtime {

class IoBackend;

class CompletionBackend {
public:
    using clock = std::chrono::steady_clock;

    CompletionBackend() = default;
    CompletionBackend(const CompletionBackend&) = delete;
    CompletionBackend& operator=(const CompletionBackend&) = delete;
    virtual ~CompletionBackend() = default;

    // 由 runtime 驱动线程调用。
    // - `timeout == duration::zero()`: 必须非阻塞
    // - `timeout == duration::max()` : 允许一直阻塞到有完成事件或 wake()
    // 返回值表示本次 poll 是否实际消费或分发了至少一个完成事件。
    virtual bool poll(clock::duration timeout) = 0;

    // 由 runtime 的其它线程安全入口调用，用于打断 backend 的阻塞 poll。
    // 实现通常映射到 eventfd/pipe/IOCP post/wakeup fd 等机制。
    virtual void wake() noexcept = 0;

    // 可选的 IO 提交侧扩展点。
    // 纯 timer-only / test-only backend 可以保持默认 nullptr；
    // 真实网络 backend 则返回一个支持 submit/cancel 的实现对象。
    MCQNET_NODISCARD
    virtual IoBackend* io_backend() noexcept { return nullptr; }
};

} // namespace mcqnet::runtime

namespace mcqnet {

using runtime::CompletionBackend;

} // namespace mcqnet
