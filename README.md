# MCQNet

MCQNet 是一个面向 Windows/Linux 的纯头文件 C++20 协程式 Proactor 网络库雏形，目标是提供接近 Tokio 风格的异步编程体验，并为后续真实异步后端实现预留清晰边界。

当前仓库已经具备任务、最小运行时、时间/取消基元、内存池和网络层骨架，但还没有接入真实的 IOCP / `io_uring` backend，因此它目前更适合作为基础设施原型和后续开发起点，而不是可直接投入生产的完整网络库。

## 当前状态

- 产物形态：header-only library target（`mcqnet`）
- 语言标准：C++20
- 构建系统：xmake 3.0+
- 已完成：
  - `Task<T>` / `JoinHandle<T>` 任务原语
  - 最小单线程 `Runtime`
  - `sleep_for` / `sleep_until` / `timeout`
  - cooperative cancel 基元
  - `OperationBase` / `OperationAwaiter` 异步操作适配层
  - `ThreadLocalPool` / `FixedBlockPool` / `ObjectPool<T>`
  - `SocketAddress`、`SocketHandle`、`TcpStream`、`TcpListener` 和 socket operation 骨架
- 尚未完成：
  - 真实 Linux socket syscall 实现
  - `TcpStream` / `TcpListener` 的真实异步 IO
  - IOCP / `io_uring` completion backend

## 目录结构

```text
.
├── xmake.lua
└── mcqnet/
    ├── include/mcqnet/
    │   ├── mcqnet.h
    │   ├── config/
    │   ├── core/
    │   ├── detail/
    │   ├── memory/
    │   ├── net/
    │   ├── runtime/
    │   ├── task/
    │   └── time/
    ├── test/
    └── benchmark/
```

## 模块概览

- `memory/`: 小对象分配、跨线程归还、线程退出后的延迟安全回收
- `task/`: `Task<T>`、`JoinHandle<T>`、`spawn()`
- `runtime/`: 最小单线程 event loop、ready queue、timer queue、pending work、backend seam
- `time/`: 基于 runtime 的 `sleep_*` 和 `timeout()`
- `net/`: socket 地址/句柄、buffer view、`TcpStream` / `TcpListener`、connect/accept/read/write operation 骨架
- `detail/`: 调度绑定、操作状态机和 awaiter 适配等底层协议

## 统一入口

大多数场景直接包含：

```cpp
#include <mcqnet/mcqnet.h>
```

它会聚合当前相对稳定的公共 API。`detail/` 仍然属于内部协议层，需要按需直接包含。

## 构建

当前仓库只保留 `xmake.lua` 作为构建入口。

```bash
xmake f -m release
xmake
```

可选项：

```bash
xmake f -m release --benchmarks=y
xmake

xmake check clang.tidy
```

如需给 clangd 生成编译数据库，可执行：

```bash
xmake project -k compile_commands build
```

## 测试

```bash
xmake test
```

测试源位于 `mcqnet/test/`，当前主要覆盖：

- 内存池主路径
- 跨线程释放与回流
- 任务语义
- runtime 驱动
- timer / timeout / cancel
- 网络层骨架和 backend seam

## 最小示例

下面这段示例展示了如何把一个协程任务投递到 `Runtime`，并在任务里使用 `sleep_for()`：

```cpp
#include <chrono>
#include <mcqnet/mcqnet.h>

using namespace std::chrono_literals;

mcqnet::Task<void> worker(mcqnet::runtime::Runtime* runtime) {
    co_await mcqnet::time::sleep_for(10ms);
    runtime->stop();
}

int main() {
    mcqnet::runtime::Runtime runtime;
    auto task = worker(&runtime);

    runtime.post(task.handle());
    runtime.run();
    return 0;
}
```

如果你需要任务结果回收，也可以使用 `runtime.spawn(task)` 或 `mcqnet::task::spawn(task)` 把 `Task<T>` 桥接成 `JoinHandle<T>`。

## 设计边界

- 当前 `Runtime` 是最小单线程实现，不是线程池，也不是多 reactor 模型
- `timeout()` 目前是“到点即抛 `timed_out`”的原始 awaitable，不是任意 awaitable 的组合子
- 取消目前是协作式语义，还没有往 OS 级 IO abort 透传
- `net/` 目录已经把调用边界、结果类型和 operation 形状固定下来，但很多底层实现仍是 scaffold

## License

This project is licensed under the MIT License. See `LICENSE` for details.

## 下一步建议

如果要继续推进这个仓库，优先级通常应该是：

1. 在现有 operation 之上补 `TcpStream` / `TcpListener` 的用户向 async convenience API
2. 对接一个真实 completion backend
3. 把 connect / accept / read / write operation 从 skeleton 收束成可工作的异步路径
4. 再决定是否向多线程 runtime 或更完整的网络抽象扩展
