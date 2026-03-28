# MCQNet Project Progress

## 1. 目的

这份文档只记录“当前做到哪一步”和“接下来最值得做什么”。

它和 `project_context.md` 的区别是：

- `project_context.md` 偏静态背景、结构和接口说明
- `project_progress.md` 偏当前进度、近期变化和下一阶段判断

## 2. 当前阶段

截至目前，MCQNet 已经进入“异步基础设施基本成型，最小 runtime、timer/cancel 基元、backend seam 和网络 IO 基础抽象已落地，但真实网络后端仍未开始”的阶段。

更具体地说：

- 内存分配层仍然是当前仓库里最成熟的一部分
- 协程任务原语已经可以独立工作
- 自定义异步操作 awaiter 适配已经可用
- 最小单线程 runtime 已具备 ready queue、timer queue、stop/drain 和 pending work 语义
- `sleep` / `timeout` / cooperative cancel 已经接到 runtime 之上
- runtime 与 IO completion backend 的最小对接点已经明确
- 网络 IO 的 submit 侧协议、native socket handle 和最小 `net` 骨架已经起步
- `SocketAddress` 已经补上，并支持 IPv4 / IPv6 literal
- Linux socket 封装骨架已经补上，`TcpStream` / `TcpListener` 也有了可直接填函数体的入口
- connect / accept / read / write operation 骨架已经补上，可继续并行交接给 backend/IO 同学
- 真实 IOCP / io_uring backend 仍未开始

## 3. 已完成的核心能力

### 3.1 内存层

已完成：

- `ThreadLocalPool`
- `FixedBlockPool`
- `ObjectPool<T>`
- `MemoryResource`
- 小对象 size class 路由
- 跨线程归还
- 线程退出后的延迟安全回收

状态判断：

- 这是仓库里最稳的模块
- 已经有测试和 benchmark 覆盖主路径

### 3.2 任务层

已完成：

- `mcqnet::task::Task<T>`
- `mcqnet::task::JoinHandle<T>`
- `mcqnet::task::spawn(Task<T>)`
- `mcqnet::runtime::Runtime::spawn(Task<T>)`

已具备语义：

- 任务创建后初始挂起
- `start()` 手动启动
- `co_await task`
- `co_await join_handle`
- `wait()` / `get()` / `join()`
- 异常跨协程传播
- 在 runtime 内 await 时，`Task` / `JoinHandle` 会默认继承当前调度上下文

当前边界：

- 自由函数 `task::spawn()` 仍然是“立即启动桥接协程”
- `Runtime::spawn()` 才是“进入 runtime ready queue”的版本
- 还没有多线程 executor / work-stealing 这类更重的调度能力

### 3.3 异步操作适配层

已完成：

- `mcqnet::detail::OperationBase`
- `mcqnet::detail::OperationAwaiter`
- `make_operation_awaiter()`
- `SchedulerBinding` / `SchedulerScope`

已具备语义：

- 保存 continuation
- 支持完成、取消、同步异常传播
- 支持通过 `ScheduleFn` 注入恢复调度策略
- 支持通过 retain/release work tracker 接入 runtime pending work 记账

当前边界：

- 这层还是底层协议
- 已经有 `IoBackend` / `IoOperationBase` 这层 submit 骨架
- 还没有 accept / connect / read / write 这些具体操作对象

### 3.4 Runtime 层

已完成：

- `mcqnet::runtime::Runtime`
- `mcqnet::runtime::Handle`
- `Runtime::current()` / `Runtime::current_handle()`
- `Runtime::post()`
- `Runtime::run()`
- `Runtime::run_one()`
- `Runtime::stop()`
- `Runtime::spawn()`
- runtime 头文件与统一入口导出

已具备语义：

- 单线程 ready queue event loop
- `post()` / `Runtime::spawn()` 进入 runtime ready queue，而不是内联恢复
- `run_one()` 非阻塞推进一次调度；无 ready work 但有 backend 时会尝试一次 `poll(0)`
- `run()` 会同时考虑 ready queue、timer queue 和 completion backend
- `run()` 在 `stop()` 之后仍会等待并 drain 已计入 pending work 的恢复义务
- `Task` / `JoinHandle` / `OperationBase` 在 runtime 内 await 时可自动继承当前 runtime
- 外部线程可通过 `post()` / `stop()` 唤醒 `run()`
- 单线程驱动保护：禁止并发或重入 `run()` / `run_one()`

当前边界：

- 这是最小单线程 runtime，不是线程池，也不是多 reactor 设计
- pending work 的正式覆盖范围只包括：
  - public `post()` / `Runtime::spawn()` 提交的 continuation
  - 显式接入 `SchedulerBinding` 协议的 awaitable
- 未接入这套协议的第三方 awaitable 不会自动计入 runtime pending work

### 3.5 时间与取消基元

已完成：

- `mcqnet::runtime::CancelSource`
- `mcqnet::runtime::CancelToken`
- `mcqnet::runtime::CancelRegistration`
- `mcqnet::time::sleep_for()`
- `mcqnet::time::sleep_until()`
- `mcqnet::time::timeout()`

已具备语义：

- timer awaitable 建在现有 runtime 之上，而不是单独起一套调度模型
- `sleep_*` / `timeout()` 可隐式绑定当前 runtime，也可显式绑定 `runtime::Handle`
- timer awaitable 会计入 runtime pending work
- `timeout()` 到期后抛出 `RuntimeException { errc::timed_out }`
- 取消 `sleep_*` 会以 `operation_aborted` 恢复 waiter
- `CancelRegistration` 在析构或 reset 时会等待 in-flight callback 退出

当前边界：

- `timeout()` 目前只是一个“到点即抛 timed_out”的原始 awaitable
- 还不是“包装任意 awaitable 的 timeout 组合子”
- 取消目前仍是协作式语义，还没有往 OS 级 IO abort 传递

### 3.6 Backend 对接 seam

已完成：

- `mcqnet::runtime::CompletionBackend`
- `mcqnet::runtime::IoBackend`
- `mcqnet::runtime::IoOperationBase`
- `CompletionBackend::poll(clock::duration timeout)`
- `CompletionBackend::wake() noexcept`
- `CompletionBackend::io_backend() noexcept`

已具备语义：

- runtime 在 idle 时把阻塞等待委托给 backend `poll(timeout)`
- runtime 在 `post()` / `spawn()` / `stop()` / timer 变更时调用 `wake()`
- backend 拿到完成事件后，负责调用 `OperationBase::complete()` / `cancel()`
- IO-capable backend 还可以通过 `IoBackend::submit()` / `cancel()` 承接 submit 侧请求
- continuation 如何回到 runtime，由 await 时注入的 scheduler / work tracker 决定
- runtime 只保存 backend 的非 owning 指针，且只允许在 idle 状态切换 backend

当前边界：

- 已经有 runtime <-> backend 的驱动边界，以及 IO submit 侧扩展点
- 还没有 accept / connect / read / write 等具体 IO 操作
- 还没有任何真实 backend 实现

### 3.7 网络 IO 基础抽象

已完成：

- `mcqnet::net::SocketHandle`
- `mcqnet::net::SocketAddress`
- `mcqnet::net::SocketIoResult` / `ConnectResult` / `AcceptResult`
- `mcqnet::net::MutableBuffer` / `ConstBuffer`
- `mcqnet::net::TcpStream`
- `mcqnet::net::TcpListener`
- `mcqnet::net::linux_detail::LinuxSocketApi`
- `mcqnet::net::detail::SocketOperationBase`
- `mcqnet::net::ConnectOperation`
- `mcqnet::net::AcceptOperation`
- `mcqnet::net::ReadOperation`
- `mcqnet::net::WriteOperation`

已具备语义：

- `SocketHandle` 统一表达跨平台 native socket handle 值
- `SocketAddress` 统一表达 IPv4 / IPv6 地址及 native sockaddr 视图
- `SocketIoResult` / `ConnectResult` / `AcceptResult` 统一表达 would_block / EOF / connect-in-progress 这类 socket 层结果
- buffer view 统一表达 read/write 的“指针 + 字节数”形状
- `TcpStream` / `TcpListener` 已能承载 socket 与 runtime 绑定关系
- `TcpStream` / `TcpListener` 已补上 Linux socket 的 open/bind/listen/connect/read/write/shutdown/accept 入口
- 具体 Linux syscall 已集中到 `linux_socket_api.h`，方便单独交接给别人填实现
- connect/accept/read/write 现在都已有 operation 骨架，并固定了“同步先尝试一次、would_block 再交 backend”的模式
- operation 完成结果由 `complete_connect()` / `complete_accept()` / `complete_read()` / `complete_write()` 回填，便于后续 backend 保留 richer 状态
- `socket_operations.h` 已直接标出 `TODO(operation)` / `TODO(backend)`，交接时可以直接按代码注释推进
- `IoOperationBase` 已经把 runtime 绑定、scheduler 继承和 backend submit 串起来

当前边界：

- `linux_socket_api.h` 当前仍是 scaffold，`socket/fcntl/setsockopt/bind/listen/accept/connect/recv/send/shutdown/getsockname/getpeername`
  这些 syscall 还没有填真实函数体
- `TcpStream` / `TcpListener` 虽然已经有对应 public 方法，但现在主要作用还是把调用边界和结果类型固定下来
- 四个 operation 目前也还是 skeleton：submit 路径和结果形状已经固定，但真实 backend completion 逻辑还没有落地

## 4. 近期已经完成的整理工作

最近这轮已经完成的工程化推进：

- 新增 `mcqnet/include/mcqnet/runtime/cancel.h`
- 新增 `mcqnet/include/mcqnet/runtime/completion_backend.h`
- 新增 `mcqnet/include/mcqnet/runtime/io_backend.h`
- 新增 `mcqnet/include/mcqnet/runtime/io_operation.h`
- 新增 `mcqnet/include/mcqnet/net/` 目录与最小 `TcpStream` / `TcpListener` / `SocketHandle` / `SocketAddress` / buffer 头
- 新增 `mcqnet/include/mcqnet/net/socket_result.h`
- 新增 `mcqnet/include/mcqnet/net/linux_socket_api.h`
- 新增 `mcqnet/include/mcqnet/net/socket_operations.h`
- 新增 `mcqnet/include/mcqnet/time/sleep.h`
- 新增 `mcqnet/include/mcqnet/time/timeout.h`
- `mcqnet/include/mcqnet/mcqnet.h` 已统一导出 runtime / cancel / time / net 公共 API
- `SchedulerBinding` 增加 `retain_work()` / `release_work()`，并明确 pending work 接入约束
- `OperationAwaiter` 现在支持操作对象显式补 runtime 绑定
- runtime 内部增加 timer queue、当前 runtime 作用域和 backend 唤醒/轮询逻辑
- `TcpStream` / `TcpListener` 已补齐 Linux socket public 槽位，后续交接应优先填 `linux_socket_api.h` 中的 `TODO(socket)`
- socket operation 骨架已经补齐，后续 backend 实现应优先对接四个 `complete_*()` helper，而不是裸写 `finish()`
- socket operation 的实际实现现在已经集中在 `socket_operations.h`
- `socket_operations.h` 里的 `submit()` / `complete_*()` / `await_resume()` 已补上待实现注释，backend/IO 同学可直接按 `TODO(operation)` / `TODO(backend)` 分工
- 新增 `test_runtime` / `test_time` / `test_net`

这意味着：

- runtime 之上的最小时间与取消基元已经有了
- 网络 IO 这一层已经不再是纯文档设想，而是有了可编译的 submit/buffer/socket 骨架
- pending work 的边界已经从“隐含规则”变成了“显式协议”
- 现在更适合继续做 IO 提交协议与网络层，而不是继续围绕 runtime 本体返工

## 5. 当前验证状态

本地已通过：

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`

当前测试共 `8/8 passed`，覆盖重点包括：

- 内存池主路径
- 跨线程释放与回流
- 线程退出后的安全释放
- `Task` / `JoinHandle` / `spawn` 协程语义
- `OperationAwaiter` 的提交、完成恢复、同步异常传播
- runtime ready queue、跨线程唤醒、单线程驱动保护
- runtime pending work / `run()` 退出条件
- 未接入协议的 awaitable 不计入 pending work
- 已接入协议的 awaitable / timer 会让 `run()` 保持存活
- completion backend 路径可以唤醒并恢复 runtime 驱动的操作
- `sleep_*` / `timeout()` / cancel 语义
- `IoBackend` / `IoOperationBase` 的 submit/cancel 骨架
- `SocketHandle` / `SocketAddress` / buffer / `TcpStream` / `TcpListener` 的基础对象语义
- Linux socket 结果类型与 `TcpStream` / `TcpListener` 新增方法签名
- 四个 socket operation 的构造、accessor、结果回填 helper 和直接 await 形状

## 6. 当前缺口

还没有进入实现阶段的关键模块：

- 真实的 socket / listener / stream 异步操作
- IOCP / io_uring backend 的真实实现
- 取消从 `CancelToken` 继续向未来 IO 操作传播
- `linux_socket_api.h` 中各个 syscall 的真实 Linux 语义
- operation 与真实 backend 的 completion 回填逻辑
- 更高层的时间组合子，例如 `interval` 或“为任意 awaitable 加 timeout”

这几个缺口里，最优先的已经不再是“补 runtime 本体”，而是把 runtime / timer / cancel / backend seam 接到真正的 IO 操作对象上。

## 7. 下一阶段建议

建议的优先级顺序：

1. 在现有 `IoOperationBase` 之上补具体 accept / connect / read / write 操作
2. 在 `SocketAddress` 之上补 `TcpListener` / `TcpStream` 的真实异步方法
3. 先接一个真实 backend（Linux 优先 `io_uring`，Windows 优先 IOCP）
4. 再往上补更高层的时间组合子和网络便利 API

原因：

- runtime 的驱动边界已经稳定，submit 侧骨架也已经有了
- 现在真正缺的是“具体 submit 什么 IO 操作、如何映射到 socket/address 语义”
- 先做最小 `net` API，才能验证 cancel / timeout / pending work 在真实 IO 上的语义是否闭合

## 8. 一句话判断

MCQNet 现在已经不是“只有内存池和协程任务”的仓库了，而是一个已经具备最小 runtime、timer/cancel 基元、backend seam 和网络 IO 基础抽象的纯头文件 C++20 协程异步基础设施仓库。

下一步的决定性工作，不是继续补 runtime，而是把具体 socket/address 操作和真实 backend 做出来。
