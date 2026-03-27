# MCQNet Project Context

## 1. 项目概述 (Project Overview)

MCQNet 是一个面向 Windows/Linux 的纯头文件 C++20 协程式 Proactor 网络库雏形，目标是提供接近 Tokio 风格的异步编程体验，并在后续对接真实异步后端（IOCP / io_uring）。当前仓库仍处于基础设施阶段，已经落地的主要是配置层、错误层、内存分配层、协程任务原语，以及一套最小化的异步操作 awaiter 适配层；真正的 runtime、executor、socket API 和后端实现尚未进入代码库。

当前代码的重心已经不再只是“内存池基础设施”：

- `mcqnet::task::Task<T>` / `Task<void>` 可以实例化、`start()`、`co_await`
- `mcqnet::task::JoinHandle<T>` / `JoinHandle<void>` 可以 `co_await`、`wait()`、`get()`、`join()`
- `mcqnet::task::spawn(Task<T>)` 可以把 `Task` 桥接成 `JoinHandle`
- `OperationBase` + `OperationAwaiter` 可以把自定义异步操作接入协程

## 2. 技术栈与构建 (Tech Stack & Build)

- 语言：C++20
- 协程模型：标准 C++20 Coroutines（`std::coroutine_handle` / promise / awaiter）
- 并发与同步：`thread_local`、原子变量、条件变量、互斥锁、`std::thread` / `std::jthread`
- 内存：自研 `ThreadLocalPool` / `FixedBlockPool` / `ObjectPool<T>`，以及 `std::pmr::memory_resource` 适配
- 构建系统：CMake 3.20+、CTest
- 产物形态：Header-only `INTERFACE` library（`mcqnet::mcqnet`）
- 顶层可选项：
  - `MCQNET_BUILD_BENCHMARKS=ON`：构建 benchmark
  - `MCQNET_ENABLE_CLANG_TIDY=ON`：构建时启用 `clang-tidy`
- 当前本地验证环境：Linux 构建目录 `build-clang-tidy/`

## 3. 模块划分与当前状态 (Architecture)

- `config/`
  - 提供平台、编译器、语言特性与断言宏
  - 关键宏包括 `MCQNET_PLATFORM_*`、`MCQNET_COMPILER_*`、`MCQNET_HAS_COROUTINE`、`MCQNET_ASSERT`
- `core/`
  - 当前 canonical 命名空间为 `mcqnet::core`
  - 提供统一错误语义 `errc` / `error_code`
  - 提供异常层级 `Exception`、`RuntimeException`、`NetException`、`IOException`、`MemoryException`
  - 提供缓存行对齐工具 `CachePadded<T>`
- `memory/`
  - 当前最成熟的模块
  - `ThreadLocalPool` 作为统一分配入口
  - 小对象走固定 size class，跨线程释放走 `remote_free_list`
  - 大对象或高对齐需求走 fallback 分配
  - 额外提供 `ObjectPool<T>` 和 `MemoryResource`
- `detail/`
  - `OperationBase`：异步操作状态机、续体保存、调度回调注入
  - `OperationAwaiter`：把符合接口的操作对象包装成 awaiter
- `task/`
  - 当前 canonical 命名空间为 `mcqnet::task`
  - `Task<T>` / `Task<void>`：轻量级协程任务对象
  - `JoinHandle<T>` / `JoinHandle<void>`：任务结果同步原语
  - `spawn(Task<T>)`：把 `Task` 桥接为可等待/可阻塞 join 的 `JoinHandle`
- `test/`
  - 已覆盖内存池主路径、线程退出安全、跨线程归还回流、协程语义
- `benchmark/`
  - 已提供小对象分配和 PMR 对比基准，但默认不构建

## 4. 对外头文件与目录情况 (Public Surface)

当前统一入口头 `mcqnet/include/mcqnet/mcqnet.h` 导出：

- `core/error.h`
- `core/exception.h`
- `core/cacheline.h`
- `task/spawn.h`
- `memory/fixed_block_pool.h`
- `memory/object_pool.h`
- `memory/thread_local_pool.h`

这意味着：

- `mcqnet::task::Task`、`JoinHandle`、`spawn()` 已经由统一入口对外暴露
- 根命名空间 `mcqnet::Task` / `JoinHandle` / `spawn` 目前仍作为兼容导出存在
- `detail/operation_base.h` 和 `detail/operation_awaiter.h` 仍属于细节层，需要直接 include
- `memory/memory_resource.h` 目前没有被 `mcqnet.h` 统一导出，使用 `MemoryResource` 需要单独包含

精简目录树：

```text
.
├── CMakeLists.txt
├── project_context.md
├── project_progress.md
├── mcqnet/
│   ├── CMakeLists.txt
│   ├── include/mcqnet/
│   │   ├── mcqnet.h
│   │   ├── config/
│   │   ├── core/
│   │   ├── detail/
│   │   │   ├── macro.h
│   │   │   ├── operation_awaiter.h
│   │   │   └── operation_base.h
│   │   ├── task/
│   │   │   ├── join_handle.h
│   │   │   ├── spawn.h
│   │   │   └── task.h
│   │   └── memory/
│   │       ├── fixed_block_pool.h
│   │       ├── memory_resource.h
│   │       ├── object_pool.h
│   │       └── thread_local_pool.h
│   ├── test/
│   │   ├── CMakeLists.txt
│   │   ├── test_coroutine.cpp
│   │   ├── test_cross_thread_alloc.cpp
│   │   ├── test_step1.cpp
│   │   └── test_step2.cpp
│   └── benchmark/
│       ├── benchmark_memory_resource.cpp
│       └── benchmark_small_object_alloc.cpp
└── build-clang-tidy/
```

## 5. 核心数据流 (Key Data Flows)

### 5.1 小对象分配与释放

- `ThreadLocalPool::allocate(size, align)`
- 若 `align <= alignof(std::max_align_t)` 且 `size <= 4096`，则路由到对应 size class 的 `FixedBlockPool`
- `FixedBlockPool` 从当前线程的 `local_free_list` 取块；为空时先 drain `remote_free_list`，再按需分配新 chunk
- 返回给用户的是 `AllocationPrefix` 后面的用户区，释放时通过 prefix 反查 `AllocationHeader`

### 5.2 跨线程归还

- 用户在任意线程调用 `ThreadLocalPool::deallocate()` / `destroy()`
- 头部中的 `AllocationHeader.owner` 指向拥有该块的 `PoolControl`
- 同线程归还直接回 `local_free_list`
- 跨线程归还以无锁方式 push 到 `remote_free_list`
- owner 线程下次分配时批量 drain `remote_free_list`

### 5.3 线程退出后的安全回收

- `FixedBlockPool` 本体析构时不会强制等待所有外部块归还
- `PoolControl::ref_count` 跟踪控制块生命周期
- `ChunkControl::live_count` 跟踪 chunk 中尚未归还的块数
- 退休后的 pool 进入 `retired` 状态，后续归还不再进入 freelist，而是按 `live_count` 归零时延迟回收 chunk

### 5.4 Task 与 continuation

- `Task` 协程创建后先挂起，`initial_suspend()` 返回 `std::suspend_always`
- 调用 `Task::start()` 会恢复协程
- `co_await task` 时，`await_suspend()` 会把父协程句柄写入 promise 的 continuation
- `final_suspend()` 通过 `FinalAwaiter` 恢复 continuation，若没有 continuation 则返回 `std::noop_coroutine()`
- 协程帧分配使用 `ThreadLocalPool::local()`，不是全局 `operator new`

### 5.5 spawn 桥接路径

- `spawn(Task<T>)` 要求传入有效 `Task`；debug 构建下会断言 `task.valid()`
- `spawn()` 内部创建共享的 `JoinState<T>`
- 然后启动一个内部 `DetachedTask` 桥接协程
- 桥接协程 `co_await std::move(task)`，把结果或异常转存到 `JoinState<T>`
- 调用方拿到的 `JoinHandle<T>` 可以：
  - 作为 awaitable 被 `co_await`
  - 通过 `wait()` / `get()` / `join()` 做阻塞等待

### 5.6 Operation awaiter 路径

- `make_operation_awaiter(operation)` 返回 `OperationAwaiter<TOperation>`
- `await_suspend()` 先调用 `operation.prepare_await(continuation)`
- 若成功进入等待态，再调用 `operation.submit()`
- 若 `submit()` 同步抛异常，则通过 `complete_inline_exception()` 捕获，并让当前协程不真正挂起
- 之后 `await_resume()` 调回具体 operation 的 `await_resume()`，由其决定返回值和异常传播

## 6. 关键接口 (Core Interfaces)

### 6.1 错误与异常

```cpp
namespace mcqnet::core {

enum class errc : std::uint32_t {
    success = 0,
    unknown,
    invalid_argument,
    invalid_state,
    not_supported,
    already_exists,
    not_found,
    permission_denied,
    operation_aborted,
    timed_out,
    cancelled,
    end_of_file,
    runtime_not_initialized,
    runtime_stopped,
    scheduler_overloaded,
    task_cancelled,
    task_join_failed,
    out_of_memory,
    pool_exhausted,
    buffer_overflow,
    address_in_use,
    address_not_available,
    connection_refused,
    connection_reset,
    connection_aborted,
    not_connected,
    already_connected,
    network_unreachable,
    host_unreachable,
    broken_pipe,
    message_too_large,
    would_block,
    io_error,
    short_read,
    short_write,
    iocp_error,
    uring_error,
    driver_error
};

constexpr std::string_view to_string(errc ec) noexcept;

struct error_code {
    errc value { errc::success };
    std::uint32_t native { 0 };

    constexpr explicit operator bool() const noexcept;
    constexpr bool success() const noexcept;
    constexpr std::string_view message() const noexcept;
};

class Exception : public std::runtime_error { ... };
class RuntimeException : public Exception { ... };
class NetException : public Exception { ... };
class IOException : public Exception { ... };
class MemoryException : public Exception { ... };

[[noreturn]] void throw_runtime_error(error_code ec, std::string msg);
[[noreturn]] void thorw_runtime_error(error_code ec, std::string msg);
[[noreturn]] void throw_net_error(error_code ec, std::string msg);
[[noreturn]] void throw_io_error(error_code ec, std::string msg);
[[noreturn]] void throw_memory_error(error_code ec, std::string msg);

} // namespace mcqnet::core
```

备注：

- `thorw_runtime_error` 这个拼写错误的兼容接口目前仍然存在
- `error_code` 是轻量按值类型，`operator bool()` 的语义与 `std::error_code` 一致，`true` 表示有错误

### 6.2 Task / JoinHandle / spawn

```cpp
namespace mcqnet::task {

template <typename T = void>
class Task;

template <typename T = void>
class JoinHandle;

template <typename T>
JoinHandle<T> spawn(Task<T> task);

} // namespace mcqnet::task
```

语义摘要：

- `Task<T>`
  - 默认构造为空任务
  - 移动专有，禁止拷贝
  - `start()` 手动启动
  - `co_await task` 支持任务链式组合
  - `await_resume()` 返回结果或重抛协程内异常
- `JoinHandle<T>`
  - 可拷贝，底层通过 `std::shared_ptr<JoinState<T>>` 共享状态
  - `await_ready()` / `await_suspend()` / `await_resume()` 让其可直接 `co_await`
  - `wait()` / `get()` / `join()` 提供阻塞式等待接口
  - `set_scheduler()` 只影响“等待该 JoinHandle 的协程如何恢复”，不影响被桥接任务本身在哪里执行
- `spawn(Task<T>)`
  - 当前实现不做 runtime 调度
  - 内部桥接协程会在调用线程立即启动
  - 适合当前阶段用于把 `Task` 接到 `JoinHandle` 语义上

### 6.3 异步操作基元

```cpp
namespace mcqnet::detail {

enum class OperationState : std::uint8_t {
    init = 0,
    awaiting = 1,
    completed = 2,
    cancelled = 3
};

using ScheduleFn = void (*)(void* context, std::coroutine_handle<> handle) noexcept;

class OperationBase {
public:
    bool is_completed() const noexcept;
    bool is_cancelled() const noexcept;
    OperationState state() const noexcept;

    void set_scheduler(ScheduleFn schedule_fn, void* schedule_context);
    std::coroutine_handle<> continuation() const noexcept;
    std::exception_ptr completion_exception() const noexcept;

    void set_user_data(std::uintptr_t user_data) noexcept;
    std::uintptr_t user_data() const noexcept;
    void set_debug_tag(const char* debug_tag) noexcept;
    const char* debug_tag() const noexcept;

    bool prepare_await(std::coroutine_handle<> continuation) noexcept;
    void complete(std::uint32_t result, std::int32_t error = 0);
    void cancel(std::int32_t error = 0) noexcept;
    void complete_inline_exception(std::exception_ptr exception) noexcept;

    OperationBase* next { nullptr };
};

template <typename TOperation>
class OperationAwaiter;

template <typename TOperation>
OperationAwaiter<TOperation> make_operation_awaiter(TOperation& operation) noexcept;

} // namespace mcqnet::detail
```

这里的约束是“鸭子类型”风格：可被 `OperationAwaiter` 适配的操作对象需要提供 `submit()` 和 `await_resume()`，并继承或兼容 `OperationBase` 的状态机协议。

### 6.4 内存分配接口

```cpp
namespace mcqnet::memory {

class FixedBlockPool {
public:
    void initialize(std::size_t user_block_size, std::size_t blocks_per_chunk);
    std::size_t user_block_size() const noexcept;
    void* allocate();
    static void deallocate_header(AllocationHeader* header) noexcept;
};

class ThreadLocalPool {
public:
    static inline constexpr std::size_t small_object_max_size = 4096;

    static ThreadLocalPool& local();
    void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t));
    void deallocate(void* ptr) noexcept;
    std::size_t usable_size(void* ptr) const noexcept;

    template <typename T, typename... Args>
    T* make(Args&&... args);

    template <typename T>
    void destroy(T* ptr) noexcept;
};

template <typename T>
class ObjectPool {
public:
    T* create(...);
    void destroy(T* ptr) noexcept;
    using unique_ptr = std::unique_ptr<T, deleter>;
    unique_ptr make_unique(...);
};

class MemoryResource : public std::pmr::memory_resource { ... };

} // namespace mcqnet::memory
```

内存模块的固定策略：

- 小对象 size class：
  - `{16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096}`
- chunk 密度：
  - `<= 64 -> 256 blocks`
  - `<= 256 -> 128 blocks`
  - `<= 1024 -> 64 blocks`
  - 其余 `32 blocks`
- fallback 条件：
  - `size > 4096`
  - 或 `align > alignof(std::max_align_t)`

备注：

- `ObjectPool<T>::make_unique()` 已经是 canonical 名称，`make_unqiue()` 仍保留兼容入口
- `MemoryResource` 直接适配 `ThreadLocalPool::local()`
- `McqnetMemoryResource` 目前只是 `MemoryResource` 的兼容别名

## 7. 测试与基准 (Tests & Benchmarks)

当前 `mcqnet/test/CMakeLists.txt` 会自动收集所有 `test_*.cpp` 并注册到 CTest。

现有测试：

- `test_step1`
  - 验证“创建线程已经退出之后，其他线程仍可安全释放该线程分配的块”
- `test_step2`
  - 覆盖 `allocate/deallocate`、`make/destroy`、跨线程释放烟测
- `test_cross_thread_alloc`
  - 精确验证 remote free 会被 owner 线程回流并复用原地址集合
- `test_coroutine`
  - 覆盖 `Task<T>`、`Task<void>`、异常传播、任务链式等待
  - 覆盖 `JoinHandle<T>` / `JoinHandle<void>` 的 await 和阻塞 join 语义
  - 覆盖 `spawn()` 对同步完成、异步完成、异常传播、`void` 任务的桥接行为
  - 覆盖 `OperationAwaiter` 的 `submit()`、完成恢复、同步异常传播

本地验证结果：

- `ctest --test-dir build-clang-tidy --output-on-failure`
- 当前为 `4/4 passed`

基准：

- `benchmark_small_object_alloc`
  - 对比 `new/delete` 与 `ObjectPool<T>`
  - 支持 round-trip 和 batched 两类工作负载
- `benchmark_memory_resource`
  - 对比 `std::pmr::new_delete_resource()`
  - 对比 `std::pmr::unsynchronized_pool_resource`
  - 对比 `mcqnet::memory::MemoryResource`
  - 覆盖 `16/64/256/4096/8192` 字节和 `64` 字节对齐场景

## 8. 当前边界与下一步 (Current Gaps & Next Steps)

当前已经可用的内容：

- 内存池主路径与跨线程回收模型
- 基础错误/异常层
- 可工作的协程任务对象 `Task`
- 可工作的任务桥接与同步原语 `JoinHandle`
- 可工作的自定义异步操作 awaiter 适配层

当前仍然缺失的内容：

- 正式的 `Runtime` / `Executor` / `Scheduler` 类型
- socket、listener、stream、buffer 等网络 API
- IOCP / io_uring backend 抽象与实现
- 取消令牌、超时、任务调度策略、跨线程投递队列
- 基于 runtime 的真正异步 `spawn` / 调度提交语义

当前值得注意的边界：

- `spawn()` 现在是“立即启动桥接协程”，不是“提交到 runtime”
- `JoinHandle::set_scheduler()` 只决定 continuation 如何恢复，不负责任务执行调度
- `OperationBase` / `OperationAwaiter` 仍位于 `mcqnet::detail`
- `MemoryResource` 还没有被 `mcqnet.h` 统一导出
- 仓库仍保留少量命名/拼写粗糙点，如 `thorw_runtime_error`、`make_unqiue`

建议的后续工作顺序：

1. 明确 runtime / executor 抽象边界
2. 设计跨线程调度与任务提交接口
3. 在 `OperationBase` 之上接入实际 IO 后端
4. 再往上构建 socket / stream / listener 等网络层 API
