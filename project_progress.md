# MCQNet Project Progress

## 1. 目的

这份文档只记录“当前做到哪一步”和“接下来最值得做什么”。

它和 `project_context.md` 的区别是：

- `project_context.md` 偏静态背景、结构和接口说明
- `project_progress.md` 偏当前进度、近期变化和下一阶段判断

## 2. 当前阶段

截至目前，MCQNet 已经进入“异步基础设施成型，最小 runtime 已落地，但网络后端和高层异步原语尚未开始”的阶段。

更具体地说：

- 内存分配层已经是当前仓库里最成熟的一部分
- 协程任务原语已经可以独立工作
- 自定义异步操作 awaiter 适配已经可用
- 最小单线程 runtime 已经具备基本调度和退出语义
- 命名风格已经开始向 Tokio 语义 + C++ API 形状收敛
- timer / timeout / socket / backend 仍未开始

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

已具备语义：

- 任务创建后初始挂起
- `start()` 手动启动
- `co_await task`
- `co_await join_handle`
- `wait()` / `get()` / `join()`
- 异常跨协程传播

当前边界：

- `spawn()` 仍然是“立即启动桥接协程”
- 还不是“提交到 runtime”

### 3.3 异步操作适配层

已完成：

- `mcqnet::detail::OperationBase`
- `mcqnet::detail::OperationAwaiter`
- `make_operation_awaiter()`

已具备语义：

- 保存 continuation
- 支持完成、取消、同步异常传播
- 支持通过 `ScheduleFn` 注入恢复调度策略

当前边界：

- 这层还是底层协议
- 还没有真实 IO backend 把它接起来

### 3.4 Runtime 层

已完成：

- `mcqnet::runtime::Runtime`
- `mcqnet::runtime::Handle`
- `Runtime::post()`
- `Runtime::run()`
- `Runtime::run_one()`
- `Runtime::stop()`
- `Runtime::spawn()`
- runtime 头文件与统一入口导出

已具备语义：

- 单线程 ready queue event loop
- `post()` / `spawn()` 进入 runtime ready queue，而不是内联恢复
- `run_one()` 非阻塞推进一次调度
- `run()` 阻塞等待 ready work，并在 `stop()` 后 drain 剩余工作
- `Task` / `JoinHandle` / `OperationBase` 在 runtime 内 await 时自动继承当前 runtime
- 外部线程可通过 `post()` / `stop()` 唤醒 `run()`
- 单线程驱动保护：禁止并发或重入 `run()` / `run_one()`
- pending work 记账：`run()` 只会在 ready queue 为空且未完成恢复义务为 0 时退出

当前边界：

- 这是最小单线程 runtime，不是线程池，也不是多 reactor 设计
- pending work 只覆盖当前库内已接入 runtime 的 await 路径
- 还没有 timer / timeout / cancel / socket / backend
- 自由函数 `task::spawn()` 仍保留其轻量桥接语义，不是 runtime owned spawn

## 4. 近期已经完成的整理工作

最近这轮已经完成的工程化整理：

- 任务层 canonical 命名空间改为 `mcqnet::task`
- 错误/异常/cachline 工具 canonical 命名空间改为 `mcqnet::core`
- 新增 `mcqnet/include/mcqnet/runtime/` 目录
- 新增 `mcqnet/include/mcqnet/task/` 目录
- 删除了旧的 `mcqnet/include/mcqnet/coroutine/` 兼容头目录
- `ObjectPool<T>::make_unique()` 成为正式名称
- `mcqnet::memory::MemoryResource` 成为正式名称
- 根命名空间仍保留一层兼容导出，减少当前调用点迁移成本
- runtime 注释、调度作用域、pending work 和测试已经同步到 canonical 命名

这意味着：

- 命名方向已经基本定住
- 现在更适合继续做 runtime 之上的 timer / backend / network，而不是继续做命名层来回折腾

## 5. 当前验证状态

本地已通过：

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`

当前测试覆盖重点：

- 内存池主路径
- 跨线程释放与回流
- 线程退出后的安全释放
- `Task` / `JoinHandle` / `spawn` 协程语义
- `OperationAwaiter` 的提交、完成恢复、同步异常传播
- runtime 头文件与 canonical include
- runtime ready queue、跨线程唤醒、单线程驱动保护
- runtime pending work / `run()` 退出条件

## 6. 当前缺口

还没有进入实现阶段的关键模块：

- timer / timeout / cancel 基元
- socket / listener / stream API
- IOCP / io_uring backend 抽象与实现
- runtime 与真实 IO completion 队列的对接
- runtime pending work 对第三方自定义 awaitable 的正式约束或扩展方案

这几个缺口里，最优先的已经不再是“把 runtime 从 0 写出来”，而是把 runtime 上层基元接上去。

## 7. 下一阶段建议

建议的优先级顺序：

1. 在现有 runtime 之上补 timer / timeout / cancel 基元
2. 明确哪些 awaitable 计入 runtime pending work，以及对应接入约束
3. 设计 runtime 与 IO completion backend 的对接点
4. 再做 socket / listener / stream API
5. 最后接 IOCP / io_uring backend 实现

原因：

- runtime 主体已经有了，再往下做 timer 和 backend 才不会反复返工
- 没有明确的 pending work 与退出语义，后续 timeout / socket / backend 很容易出现悬挂或早退

## 8. 一句话判断

MCQNet 现在已经越过“只有想法和内存池”的阶段，也越过了“runtime 还不存在”的阶段，但还没有进入“真正可运行的异步网络库”阶段。

更准确地说，它现在是：

- 一个已经具备最小 runtime、任务层和恢复语义的
- 纯头文件 C++20 协程异步基础设施仓库

下一步的决定性工作不是继续补工具，而是把 runtime 之上的 timer / backend / network 层做出来。
