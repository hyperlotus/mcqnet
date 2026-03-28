#pragma once

// IO 提交侧 backend 协议。
// runtime::CompletionBackend 只解决“谁来 poll/wake 完成队列”；
// 这里补上“具体 IO 操作如何提交/取消”的统一入口。

#include <mcqnet/detail/macro.h>

namespace mcqnet::runtime {

class IoOperationBase;

class IoBackend {
public:
    IoBackend() = default;
    IoBackend(const IoBackend&) = delete;
    IoBackend& operator=(const IoBackend&) = delete;
    virtual ~IoBackend() = default;

    // 提交一个已经绑定好 runtime/scheduler 的 IO 操作。
    // backend 需要在未来某个时刻通过 IoOperationBase::finish()/finish_cancelled()
    // 或其更上层语义包装，把结果送回等待方。
    virtual void submit(IoOperationBase& operation) = 0;

    // best-effort 取消请求。
    // backend 可以同步完成取消，也可以在之后某个完成事件里把操作收束掉。
    virtual void cancel(IoOperationBase& operation) noexcept = 0;
};

} // namespace mcqnet::runtime

namespace mcqnet {

using runtime::IoBackend;

} // namespace mcqnet
