# MCQNet Project Context

## 1. 项目概述 (Project Overview)

MCQNet 是一个面向 Windows/Linux 的纯头文件 C++20 协程式 Proactor 网络库雏形，目标是提供接近 Tokio 风格的异步编程体验，并在后续对接真实异步后端（IOCP / io_uring）。

当前仓库已经不再只是“内存池 + 协程玩具”阶段。已经落地的能力包括：

- `mcqnet::task::Task<T>` / `JoinHandle<T>` 任务原语
- 最小单线程 `mcqnet::runtime::Runtime`
- runtime 之上的 timer / timeout / cooperative cancel 基元
- `OperationBase` + `OperationAwaiter` 异步操作适配层
- runtime 与未来 IO completion backend 的最小对接 seam
- 网络 IO 的 submit 侧协议与最小 `net` 骨架
- `SocketAddress` 与 IPv4 / IPv6 literal 地址表达
- Linux socket syscall 封装骨架，以及 `TcpStream` / `TcpListener` 的同步/非阻塞入口
- connect / accept / read / write operation 骨架

当前尚未进入代码库的仍然是网络层与真实后端：

- `TcpListener` / `TcpStream` / `UdpSocket` 的真实异步方法
- `linux_socket_api.h` 中各个 TODO(socket) 对应的真实 Linux syscall 语义
- operation 与真实 completion backend 的完整收束逻辑
- IOCP / io_uring 的真实 backend 实现

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
- 当前本地验证环境：
  - 日常构建目录 `build`
  - 可选静态检查目录 `build-clang-tidy/`

## 3. 模块划分与当前状态 (Architecture)

- `config/`
  - 提供平台、编译器、语言特性与断言宏
  - 关键宏包括 `MCQNET_PLATFORM_*`、`MCQNET_COMPILER_*`、`MCQNET_HAS_COROUTINE`、`MCQNET_ASSERT`
- `core/`
  - canonical 命名空间为 `mcqnet::core`
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
  - `SchedulerBinding` / `SchedulerScope`：表达“continuation 应该调度到哪里去，以及是否计入 pending work”
  - `OperationBase`：异步操作状态机、续体保存、调度回调注入
  - `OperationAwaiter`：把符合接口的操作对象包装成 awaiter
- `task/`
  - canonical 命名空间为 `mcqnet::task`
  - `Task<T>` / `Task<void>`：轻量级协程任务对象
  - `JoinHandle<T>` / `JoinHandle<void>`：任务结果同步原语
  - `spawn(Task<T>)`：立即启动桥接协程，把 `Task` 接成可等待/可阻塞 join 的 `JoinHandle`
- `runtime/`
  - canonical 命名空间为 `mcqnet::runtime`
  - `Runtime`：最小单线程 event loop，维护 ready queue、timer queue 和 pending work
  - `Handle`：对已有 runtime 的轻量引用视图
  - `CancelSource` / `CancelToken` / `CancelRegistration`：协作式取消基元
  - `CompletionBackend`：runtime 与未来 IO completion backend 的最小驱动边界
  - `IoBackend` / `IoOperationBase`：IO submit 侧协议与基础操作对象
- `net/`
  - canonical 命名空间为 `mcqnet::net`
  - `SocketHandle`：跨平台 native socket handle 的统一包装
  - `SocketAddress`：统一承载 IPv4 / IPv6 地址及 native sockaddr 视图
  - `SocketIoResult` / `ConnectResult` / `AcceptResult`：socket 层统一结果结构
  - `MutableBuffer` / `ConstBuffer`：网络 IO 的轻量 buffer view
  - `TcpStream` / `TcpListener`：当前承载 socket + runtime 绑定关系，以及 Linux socket public 入口
  - `linux_detail::LinuxSocketApi`：集中承接 Linux syscall / sockopt / errno 映射的内部骨架
  - `detail::SocketOperationBase`：四类 socket operation 的公共基类
  - `ConnectOperation` / `AcceptOperation` / `ReadOperation` / `WriteOperation`：socket async operation 骨架
  - `socket_operations.h` 中已直接标出 `TODO(operation)` / `TODO(backend)`，便于交接实现
- `time/`
  - canonical 命名空间为 `mcqnet::time`
  - `sleep_for()` / `sleep_until()`：基于 runtime timer queue 的 sleep awaitable
  - `timeout()`：一个“到点即抛 `timed_out`”的原始 awaitable
- `test/`
  - 已覆盖内存池主路径、线程退出安全、跨线程归还回流、任务语义、runtime、timer、cancel
- `benchmark/`
  - 已提供小对象分配和 PMR 对比基准，但默认不构建

## 4. 对外头文件与目录情况 (Public Surface)

当前统一入口头 `mcqnet/include/mcqnet/mcqnet.h` 直接聚合：

- `core/cacheline.h`
- `core/error.h`
- `core/exception.h`
- `detail/macro.h`
- `memory/fixed_block_pool.h`
- `memory/object_pool.h`
- `memory/thread_local_pool.h`
- `net/buffer.h`
- `net/socket_address.h`
- `net/socket_handle.h`
- `net/socket_operations.h`
- `net/socket_result.h`
- `net/tcp_listener.h`
- `net/tcp_stream.h`
- `runtime/cancel.h`
- `runtime/io_backend.h`
- `runtime/io_operation.h`
- `runtime/runtime.h`
- `task/spawn.h`
- `time/sleep.h`
- `time/timeout.h`

补充说明：

- `runtime/runtime.h` 内部会进一步包含 `runtime/completion_backend.h`
- `task/spawn.h` 内部会带出 `task/task.h` 与 `task/join_handle.h`
- 因此统一入口已经能拿到 task / runtime / cancel / time / net 这几层主要公共 API
- `detail/operation_base.h` 和 `detail/operation_awaiter.h` 仍属于细节层，需要按需直接 include
- `memory/memory_resource.h` 目前仍没有被 `mcqnet.h` 统一导出

命名空间暴露现状：

- `mcqnet::Task` / `mcqnet::JoinHandle` / `mcqnet::spawn` 仍作为 task 层兼容导出存在
- `mcqnet::CancelSource` / `mcqnet::CancelToken` / `mcqnet::CancelRegistration` 也有根命名空间兼容导出
- `mcqnet::sleep_for` / `mcqnet::sleep_until` / `mcqnet::timeout` 也有根命名空间兼容导出
- `mcqnet::SocketHandle` / `mcqnet::SocketAddress` / `mcqnet::TcpStream` / `mcqnet::TcpListener` 也有根命名空间兼容导出
- `mcqnet::SocketIoResult` / `mcqnet::ConnectResult` / `mcqnet::AcceptResult` / `mcqnet::SocketShutdownMode` 也有根命名空间兼容导出
- `mcqnet::ConnectOperation` / `mcqnet::AcceptOperation` / `mcqnet::ReadOperation` / `mcqnet::WriteOperation` 也有根命名空间兼容导出
- `Runtime` / `Handle` 当前仍以 `mcqnet::runtime::Runtime` / `mcqnet::runtime::Handle` 为正式路径

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
│   │   │   ├── operation_base.h
│   │   │   └── scheduler.h
│   │   ├── memory/
│   │   │   ├── fixed_block_pool.h
│   │   │   ├── memory_resource.h
│   │   │   ├── object_pool.h
│   │   │   └── thread_local_pool.h
│   │   ├── net/
│   │   │   ├── buffer.h
│   │   │   ├── linux_socket_api.h
│   │   │   ├── socket_address.h
│   │   │   ├── socket_handle.h
│   │   │   ├── socket_operations.h
│   │   │   ├── socket_result.h
│   │   │   ├── tcp_listener.h
│   │   │   └── tcp_stream.h
│   │   ├── runtime/
│   │   │   ├── cancel.h
│   │   │   ├── completion_backend.h
│   │   │   ├── io_backend.h
│   │   │   ├── io_operation.h
│   │   │   └── runtime.h
│   │   ├── task/
│   │   │   ├── join_handle.h
│   │   │   ├── spawn.h
│   │   │   └── task.h
│   │   └── time/
│   │       ├── sleep.h
│   │       └── timeout.h
│   ├── test/
│   │   ├── CMakeLists.txt
│   │   ├── test_coroutine.cpp
│   │   ├── test_cross_thread_alloc.cpp
│   │   ├── test_runtime.cpp
│   │   ├── test_runtime_headers.cpp
│   │   ├── test_step1.cpp
│   │   ├── test_step2.cpp
│   │   └── test_time.cpp
│   └── benchmark/
│       ├── benchmark_memory_resource.cpp
│       └── benchmark_small_object_alloc.cpp
└── build/
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

### 5.5 `task::spawn()` 与 `Runtime::spawn()`

- `task::spawn(Task<T>)` 要求传入有效 `Task`
- 自由函数 `task::spawn()` 会立即启动内部桥接协程，不经过 runtime ready queue
- `Runtime::spawn(Task<T>)` 会把 bridge 协程首个 resume 放进 runtime ready queue
- `Runtime::spawn()` 返回的 `JoinHandle` continuation 也会默认回到该 runtime

### 5.6 runtime 驱动与 pending work

- `Runtime::post()` / `Runtime::spawn()` 会把 continuation 放入 ready queue，并新增一份 pending work
- `run()` / `run_one()` 在恢复 continuation 时，会安装当前 runtime 作用域和 `SchedulerScope`
- `Task` / `JoinHandle` / `OperationBase` 在 await 时若未显式绑定 scheduler，会继承当前 `SchedulerBinding`
- runtime pending work 的正式覆盖范围是：
  - public `post()` / `Runtime::spawn()` 提交的 continuation
  - 显式接入 `SchedulerBinding` 协议的 awaitable
- 这组协议的要求是：
  - await 前拿到 `schedule_fn`
  - 真正异步挂起前 `retain_work()`
  - 异步完成时通过 `schedule_fn` 回到 runtime
  - 恢复义务被 ready item 消费后 `release_work()`
- 未接入这套协议的第三方 awaitable 不会自动计入 runtime pending work

### 5.7 `run()` / `run_one()` / timer queue / backend

- `run_one()` 非阻塞推进一次：
  - 若 ready queue 有 work，则执行一个
  - 若没有 ready work 但存在 pending work 且配置了 backend，则尝试一次 `backend->poll(0)`
- `run()` 会阻塞等待，直到同时满足：
  - `stop()` 已被请求
  - ready queue 已空
  - pending work 为 0
- 当 ready queue 为空但仍有 timer 或 backend pending work 时：
  - runtime 会先计算最近的 timer deadline
  - 若配置了 backend，则把等待时间交给 `backend->poll(timeout)`
  - 若没有 backend，则用条件变量等待

### 5.8 timer / timeout / cancel 路径

- `sleep_for()` / `sleep_until()` / `timeout()` 基于 `BasicTimerOperation` 构建
- await 时会先显式或隐式绑定 runtime，再绑定当前 scheduler/work tracker
- 真正挂起前先 `retain_work()`
- `submit()` 要么：
  - 直接在 deadline 已到时同步完成
  - 要么向 runtime timer queue 注册一个 callback
- 取消路径通过 `CancelRegistration` 监听 `CancelToken`
- 取消 timer 时：
  - 会尝试从 runtime timer queue 移除 timer
  - 然后以 `operation_aborted` 恢复 waiter
- `timeout()` 超时时不会正常完成，而是以 `timed_out` 语义恢复，并在 `await_resume()` 中抛出 `RuntimeException`

### 5.9 runtime 与 completion backend 的对接

- runtime 只知道两件事：
  - idle 时调用 `CompletionBackend::poll(timeout)`
  - 本地有新事件时调用 `CompletionBackend::wake()`
- backend 自己管理平台完成队列
- backend 在拿到完成事件后，负责调用 `OperationBase::complete()` / `cancel()`
- continuation 最终回到哪个执行上下文，不由 backend 决定，而由 await 时注入的 `schedule_fn` 决定
- 若 backend 还实现了 `IoBackend`，则还可以通过 `submit()` / `cancel()` 承接 IO 提交侧请求

### 5.10 网络 IO 基础抽象

- `SocketHandle` 用统一类型表达 Linux fd / Windows socket handle 这类 native 值
- `SocketAddress` 用统一类型表达 IPv4 / IPv6 地址，并保存 `sockaddr_storage + length`
- `SocketIoResult` / `ConnectResult` / `AcceptResult` 把 would_block / EOF / connect-in-progress 统一成显式结果
- `MutableBuffer` / `ConstBuffer` 把 read/write 统一成“指针 + 字节数”
- `TcpStream` / `TcpListener` 现在除了对象语义外，也提供 Linux socket 规划好的 public 方法槽位
- `linux_socket_api.h` 是 Linux native socket 的唯一承接点，负责：
  - `socket` / `close` / `fcntl` / `setsockopt` / `bind` / `listen`
  - `accept` / `connect` / `recv` / `send` / `shutdown`
  - `getsockname` / `getpeername`
  - `errno -> error_code` 映射
- `linux_socket_api.h` 明确不负责：
  - runtime pending work
  - coroutine await / suspend / resume
  - backend submit / cancel
- `SocketOperationBase` 在 `IoOperationBase` 之上补了一层共通模式：
  - 同步先尝试一次 socket 操作
  - 若结果是 `would_block`，则统一接到 backend
  - cooperative cancel -> backend cancel 的 wiring 统一收敛
- `ConnectOperation` / `AcceptOperation` / `ReadOperation` / `WriteOperation` 当前都保留 richer 结果对象在自身内部
- backend 完成路径应优先调用各自的 `complete_*()` helper，而不是直接裸调 `finish()`
- `socket_operations.h` 代码内已经直接标出：
  - `TODO(operation)`：同步尝试和协议确认的落点
  - `TODO(backend)`：真实 completion 回填和 helper 调用的落点
- `IoOperationBase` 在 `OperationBase` 之上补上：
  - 显式 runtime 绑定
  - backend submit 入口
  - cooperative cancel 注册辅助
- `OperationAwaiter` 现在会在存在该钩子时先调用 `bind_explicit_runtime_if_missing()`

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
  - `set_scheduler()` 只影响“等待该 JoinHandle 的协程如何恢复”
- `task::spawn(Task<T>)`
  - 当前实现不做 runtime 调度
  - 内部桥接协程会在调用线程立即启动

### 6.3 Runtime / Handle / CompletionBackend

```cpp
namespace mcqnet::runtime {

class CompletionBackend {
public:
    using clock = std::chrono::steady_clock;

    virtual bool poll(clock::duration timeout) = 0;
    virtual void wake() noexcept = 0;
    virtual IoBackend* io_backend() noexcept;
};

class IoBackend {
public:
    virtual void submit(IoOperationBase& operation) = 0;
    virtual void cancel(IoOperationBase& operation) noexcept = 0;
};

class Handle {
public:
    bool valid() const noexcept;
    explicit operator bool() const noexcept;
    void post(std::coroutine_handle<> continuation) const;

    template <typename T>
    task::JoinHandle<T> spawn(task::Task<T> task_value) const;
};

class Runtime {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    explicit Runtime(CompletionBackend* completion_backend = nullptr) noexcept;

    Handle handle() noexcept;

    static Runtime* current() noexcept;
    static Handle current_handle() noexcept;

    bool stopped() const noexcept;

    void set_completion_backend(CompletionBackend* completion_backend);
    CompletionBackend* completion_backend() const noexcept;

    void post(std::coroutine_handle<> continuation);
    void run();
    bool run_one();
    void stop() noexcept;

    template <typename T>
    task::JoinHandle<T> spawn(task::Task<T> task_value);
};

} // namespace mcqnet::runtime
```

语义摘要：

- `post()` / `Runtime::spawn()` 是线程安全入口
- `stop()` 后会拒绝新的 public `post()` / `spawn()`，但不会丢掉已计入 pending work 的恢复义务
- `run()` / `run_one()` 带单线程驱动保护，禁止并发或重入
- backend 由调用方持有生命周期，runtime 只保存非 owning 指针
- 只有在 runtime idle 时才允许 `set_completion_backend()`
- `CompletionBackend::io_backend()` 是可选扩展点；非 IO backend 可以返回 `nullptr`

### 6.4 CancelSource / CancelToken / CancelRegistration

```cpp
namespace mcqnet::runtime {

class CancelToken;

class CancelRegistration {
public:
    using CallbackFn = void (*)(void*) noexcept;

    CancelRegistration() noexcept = default;
    CancelRegistration(const CancelToken& token, CallbackFn callback, void* context);

    void reset() noexcept;
    void reset(const CancelToken& token, CallbackFn callback, void* context);
    bool active() const noexcept;
};

class CancelSource {
public:
    bool valid() const noexcept;
    explicit operator bool() const noexcept;
    bool stop_requested() const noexcept;
    bool cancel();
    CancelToken token() const noexcept;
};

class CancelToken {
public:
    bool valid() const noexcept;
    explicit operator bool() const noexcept;
    bool stop_requested() const noexcept;
};

} // namespace mcqnet::runtime
```

语义摘要：

- 这是协作式取消，不直接等价于 OS 级 abort
- `CancelSource::cancel()` 会触发所有已注册 callback
- `CancelRegistration::reset()` 会在回调可能正在执行时等待其退出，避免 use-after-free

### 6.5 time::sleep / timeout

```cpp
namespace mcqnet::time {

template <typename TDuration>
auto sleep_for(TDuration duration, runtime::CancelToken cancel_token = {});

template <typename TDuration>
auto sleep_for(runtime::Handle runtime_handle, TDuration duration, runtime::CancelToken cancel_token = {});

auto sleep_until(runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {});
auto sleep_until(runtime::Handle runtime_handle, runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {});

template <typename TDuration>
auto timeout(TDuration duration, runtime::CancelToken cancel_token = {});

template <typename TDuration>
auto timeout(runtime::Handle runtime_handle, TDuration duration, runtime::CancelToken cancel_token = {});

auto timeout(runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {});
auto timeout(runtime::Handle runtime_handle, runtime::Runtime::time_point deadline, runtime::CancelToken cancel_token = {});

} // namespace mcqnet::time
```

语义摘要：

- 若未显式给 `runtime::Handle`，会尝试绑定 `Runtime::current_handle()`
- 若当前线程不在 runtime 驱动作用域中，且也没有显式 handle，则抛 `runtime_not_initialized`
- `sleep_*` 正常到期后只恢复，不返回值
- `timeout()` 到期时会在 `await_resume()` 中抛出 `RuntimeException { errc::timed_out }`
- 当前 `timeout()` 不是“包装任意 awaitable”的组合子，只是一个原始 timer awaitable

### 6.6 SchedulerBinding 与异步操作基元

```cpp
namespace mcqnet::detail {

using ScheduleFn = void (*)(void* context, std::coroutine_handle<> handle) noexcept;
using RetainWorkFn = void (*)(void* context) noexcept;
using ReleaseWorkFn = void (*)(void* context) noexcept;

struct SchedulerBinding {
    ScheduleFn schedule_fn { nullptr };
    void* schedule_context { nullptr };
    RetainWorkFn retain_work_fn { nullptr };
    ReleaseWorkFn release_work_fn { nullptr };

    bool valid() const noexcept;
    bool tracks_work() const noexcept;
    void schedule(std::coroutine_handle<> continuation) const noexcept;
    void retain_work() const noexcept;
    void release_work() const noexcept;
};

class SchedulerScope {
public:
    SchedulerScope(
        ScheduleFn schedule_fn,
        void* schedule_context,
        RetainWorkFn retain_work_fn = nullptr,
        ReleaseWorkFn release_work_fn = nullptr) noexcept;

    static SchedulerBinding current() noexcept;
};

enum class OperationState : std::uint8_t {
    init = 0,
    awaiting = 1,
    completed = 2,
    cancelled = 3
};

class OperationBase {
public:
    bool is_completed() const noexcept;
    bool is_cancelled() const noexcept;
    OperationState state() const noexcept;

    void set_scheduler(ScheduleFn schedule_fn, void* schedule_context) noexcept;
    bool has_scheduler() const noexcept;

    void set_work_tracker(RetainWorkFn retain_work_fn, ReleaseWorkFn release_work_fn) noexcept;
    bool has_work_tracker() const noexcept;
    void retain_work() const noexcept;
    void release_work() const noexcept;

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

这里的关键约束是：

- `SchedulerBinding` 既表达“恢复到哪里”，也表达“是否计入 pending work”
- 想让第三方 awaitable 被 runtime 正式追踪，就必须显式接入这组协议
- `OperationAwaiter` 适配的操作对象仍是“鸭子类型”风格：
  - 需要提供 `submit()` 和 `await_resume()`
  - 并继承或兼容 `OperationBase` 的状态机协议

### 6.7 网络 IO 基础接口

```cpp
namespace mcqnet::runtime {

enum class IoOperationKind : std::uint8_t {
    accept = 0,
    connect = 1,
    receive = 2,
    send = 3
};

class IoOperationBase : public detail::OperationBase {
public:
    IoOperationKind kind() const noexcept;
    net::SocketHandle socket() const noexcept;
    Handle runtime_handle() const noexcept;
    const CancelToken& cancel_token() const noexcept;

    void bind_explicit_runtime_if_missing() noexcept;

    void finish(std::uint32_t result, std::int32_t error = 0);
    void finish_cancelled(std::int32_t error = 0) noexcept;
};

} // namespace mcqnet::runtime

namespace mcqnet::net {

class SocketHandle { ... };
class SocketAddress { ... };

struct MutableBuffer {
    void* data;
    std::size_t size;
};

struct ConstBuffer {
    const void* data;
    std::size_t size;
};

class TcpStream { ... };
class TcpListener { ... };

} // namespace mcqnet::net
```

语义摘要：

- `IoOperationBase` 是 accept/connect/read/write 这类未来具体操作的共通基类
- backend 驱动 `IoOperationBase` 时，应通过 `finish()` / `finish_cancelled()` 收束操作
- `SocketAddress` 当前支持 IPv4 / IPv6 literal 构造、解析和 native round-trip
- `TcpStream` / `TcpListener` 当前还只是对象语义骨架，不是完整 socket API

### 6.8 内存分配接口

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
  - 覆盖 `task::spawn()` 对同步完成、异步完成、异常传播、`void` 任务的桥接行为
  - 覆盖 `OperationAwaiter` 的 `submit()`、完成恢复、同步异常传播
- `test_runtime`
  - 覆盖 `Runtime::post()` / `run()` / `run_one()` / `stop()` / `Runtime::spawn()`
  - 覆盖 runtime 驱动保护、pending work 退出条件、tracked/untracked awaitable 边界
  - 覆盖 completion backend 路径
- `test_runtime_headers`
  - 覆盖 runtime 相关 public headers 的聚合可用性
- `test_time`
  - 覆盖 `sleep_until()` 恢复到 runtime
  - 覆盖 `timeout()` 抛出 `timed_out`
  - 覆盖 cancellable sleep 的 `operation_aborted`
  - 覆盖 pending timer 不会让 `run()` 在 `stop()` 后过早退出
  - 覆盖显式 `runtime::Handle` 绑定
- `test_net`
  - 覆盖 `IoBackend` / `IoOperationBase` 的 submit/cancel 骨架
  - 覆盖 `SocketHandle` / `SocketAddress` / buffer / `TcpStream` / `TcpListener` 的基础对象语义
  - 覆盖显式 runtime 绑定的 IO 操作可以通过 backend 恢复回 runtime

本地验证结果：

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- 当前为 `8/8 passed`

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
- 可工作的最小单线程 `Runtime`
- 可工作的 `sleep` / `timeout` / cooperative cancel 基元
- 可工作的自定义异步操作 awaiter 适配层
- runtime 与 completion backend 的最小对接 seam
- 网络 IO 的 submit/buffer/socket 基础抽象
- `SocketAddress` 与 IPv6 支持

当前仍然缺失的内容：

- socket、listener、stream 的真实异步方法
- IOCP / io_uring backend 抽象之上的真实实现
- bind/listen/connect 语义
- 取消向未来 IO 操作的传递
- 更高层的时间组合子，如 `interval` 或“任意 awaitable 的 timeout 包装”

当前值得注意的边界：

- `task::spawn()` 现在仍是“立即启动桥接协程”，不是“提交到 runtime”
- `Runtime::spawn()` 才是 runtime owned 的任务提交入口
- runtime pending work 只正式覆盖 public `post()` / `Runtime::spawn()` 和显式接入 `SchedulerBinding` 协议的 awaitable
- `OperationBase` / `OperationAwaiter` 仍位于 `mcqnet::detail`
- `CompletionBackend` 已经有可选的 `io_backend()` 扩展点，但还没有真实 backend 实现
- `MemoryResource` 还没有被 `mcqnet.h` 统一导出
- 仓库仍保留少量兼容性粗糙点，如 `thorw_runtime_error`、`make_unqiue`

建议的后续工作顺序：

1. 在 `IoOperationBase` 之上落具体 accept / connect / read / write 操作
2. 给 `TcpListener` / `TcpStream` 补基于 `SocketAddress` 的真实异步方法
3. 落一个真实 backend（Linux 先 `io_uring`，Windows 先 IOCP）
4. 再做更高层的 time / cancel / network 组合子与便利接口
