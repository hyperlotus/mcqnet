# Linux Socket 封装交接文档

这份文档是给接手实现 Linux socket 封装的人看的。

目标不是解释整个项目，而是把这次已经搭好的框架讲清楚，让接手的人可以直接去填函数体，不需要先重新设计一遍接口。

## 1. 这次已经搭好的东西

当前仓库里，Linux socket 这一层已经拆成了下面几块：

- [mcqnet/include/mcqnet/net/socket_address.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/socket_address.h)
  - 统一地址类型
  - 已支持 IPv4 / IPv6 literal
  - 内部保存 `sockaddr_storage + length`
  - 可直接给 `bind/connect/accept/getsockname/getpeername` 复用
- [mcqnet/include/mcqnet/net/socket_result.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/socket_result.h)
  - socket 层统一结果类型
  - 把 `would_block` / `EOF` / `connect in progress` 明确成结构体语义
- [mcqnet/include/mcqnet/net/linux_socket_api.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/linux_socket_api.h)
  - Linux syscall 封装骨架
  - 这是本次交接的主战场
  - 所有 `TODO(socket)` 都在这里
- [mcqnet/include/mcqnet/net/tcp_stream.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/tcp_stream.h)
  - 面向用户的 stream 对象
  - public 方法已经接到 `LinuxSocketApi`
- [mcqnet/include/mcqnet/net/tcp_listener.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/tcp_listener.h)
  - 面向用户的 listener 对象
  - public 方法已经接到 `LinuxSocketApi`
- [mcqnet/include/mcqnet/net/socket_operations.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/socket_operations.h)
  - 集中承载 `SocketOperationBase`、`ConnectOperation`、`AcceptOperation`、`ReadOperation`、`WriteOperation`
  - 这是 async socket operation 的主入口
  - 现在已经能直接交给别人补“同步尝试逻辑”和“backend completion 回填逻辑”
  - 代码里已经直接标了 `TODO(operation)` / `TODO(backend)`，可以按这些注释逐段填

换句话说：

- 你现在不需要重新设计 `TcpStream` / `TcpListener` 的接口
- 也不需要重新设计错误返回形状
- 只需要把 `linux_socket_api.h` 里的函数体补上
- 如果要继续往 async socket operation 往下接，也不需要重新设计 operation 形状，直接按 `socket_operations.h` 里的 `TODO(operation)` / `TODO(backend)` 继续补即可

## 2. 分层边界

这一层的边界必须守住。

### 2.1 `linux_socket_api.h` 负责什么

只负责 Linux native socket 语义：

- `socket`
- `close`
- `fcntl`
- `setsockopt`
- `bind`
- `listen`
- `accept` / `accept4`
- `connect`
- `recv` / `send`
- `shutdown`
- `getsockname`
- `getpeername`
- `errno -> error_code` 映射

### 2.2 `linux_socket_api.h` 不负责什么

不要在这里做这些事：

- 不要引入 coroutine 状态机
- 不要碰 `co_await`
- 不要接 runtime pending work
- 不要碰 `IoBackend`
- 不要在这里设计 epoll / io_uring 事件循环

这是一个纯 syscall 封装层。

后面如果要做真正的 async operation，应该建立在这层已经固定好的结果语义之上，而不是绕过去另起一套协议。

### 2.3 operation 层负责什么

`socket_operations.h` 这一层负责：

- 先同步尝试一次 socket 操作
- 若同步结果是 `would_block`，再交给 `runtime::IoBackend`
- 通过 `CancelToken` 接 cooperative cancel
- 在 `await_resume()` 返回统一结果结构

代码内的 TODO 约定：

- `TODO(operation)`
  - 需要补同步尝试语义
  - 需要确认 `would_block` / EOF / connect in progress 这些 socket 结果和上层协议是否一致
- `TODO(backend)`
  - 需要补真实 completion backend 完成路径
  - 需要在完成时调用 `complete_connect()` / `complete_accept()` / `complete_read()` / `complete_write()`

这一层不负责：

- 具体 Linux syscall
- 真实 completion backend 事件循环

也就是说：

- Linux syscall 的人主要填 `linux_socket_api.h`
- IO backend / operation 的人主要填 `socket_operations.h` 和未来 backend 的对接

最简单的交接方式就是：

- syscall 同学打开 `linux_socket_api.h`，按 `TODO(socket)` 填
- backend / operation 同学打开 `socket_operations.h`，按 `TODO(operation)` / `TODO(backend)` 填

## 3. 当前 public API 形状

### 3.1 `TcpStream`

当前已经预留了这些方法：

- `TcpStream::open(AddressFamily, runtime::Handle = {})`
- `close()`
- `set_non_blocking(bool)`
- `set_tcp_no_delay(bool)`
- `connect(const SocketAddress&)`
- `read_some(MutableBuffer)`
- `write_some(ConstBuffer)`
- `shutdown(SocketShutdownMode)`
- `local_address()`
- `peer_address()`

这些方法自己不做复杂逻辑，基本都是薄包装，最终调用 `LinuxSocketApi`。

### 3.2 `TcpListener`

当前已经预留了这些方法：

- `TcpListener::open(AddressFamily, runtime::Handle = {})`
- `close()`
- `set_non_blocking(bool)`
- `set_reuse_address(bool)`
- `set_reuse_port(bool)`
- `bind(const SocketAddress&)`
- `listen(int backlog = 128)`
- `accept_raw()`
- `local_address()`

其中 `accept_raw()` 当前直接返回 `AcceptResult`，这是故意的。

原因是后面做 async accept 的时候，可以直接把这个结果形状沿用下去，不需要重新定义一套 `accept` 状态协议。

### 3.3 async operation 形状

当前已经预留四个 operation：

- `ConnectOperation`
- `AcceptOperation`
- `ReadOperation`
- `WriteOperation`

使用方式故意收敛成统一模式：

```cpp
mcqnet::ConnectOperation op(stream, remote_address, runtime_handle, cancel_token);
mcqnet::ConnectResult result = co_await op;
```

```cpp
mcqnet::ReadOperation op(stream, mcqnet::buffer(bytes), runtime_handle, cancel_token);
mcqnet::SocketIoResult result = co_await op;
```

注意：

- `operator co_await()` 只支持左值
- 也就是要先把 operation 绑定到局部变量，再 `co_await`
- 这样做是为了避免把引用型 awaiter 绑到临时对象上

另外：

- `socket_operations.h` 里的 `submit()` / `complete_*()` / `await_resume()` 都已经有注释和 TODO
- 后续实现尽量就在这些固定位置补，不要额外拆散到别的头里

## 4. 结果类型语义

### 4.1 `SocketIoResult`

用于 `read_some` / `write_some`。

字段：

- `transferred`
- `error`

约定：

- 成功：`error == success`
- would block：`error == errc::would_block`
- EOF：`error == errc::end_of_file`

特别注意：

- `read_some()` 返回 `0` 字节且对端关闭连接时，不要当成普通成功，应该映射成 `end_of_file`
- `write_some()` 不存在 EOF 语义，常见错误应该是 `would_block`、`broken_pipe`、`connection_reset`

### 4.2 `ConnectResult`

用于 `connect()`。

字段：

- `completed`
- `error`

约定：

- 立刻连接成功：`completed = true`, `error = success`
- 非阻塞连接进行中：`completed = false`, `error = would_block`
- 失败：`completed = false`, `error = 具体错误`

这个设计是为了保留 Linux 非阻塞 `connect` 的真实语义。

不要把 `EINPROGRESS` 直接抛异常。

### 4.3 `AcceptResult`

用于 `accept()`。

字段：

- `socket`
- `peer_address`
- `error`

约定：

- 成功：`socket.valid() == true`, `error == success`
- would block：`socket` 无效，`error == would_block`
- 失败：`socket` 无效，`error = 具体错误`

## 5. 你需要填的函数

主文件：

- [mcqnet/include/mcqnet/net/linux_socket_api.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/linux_socket_api.h)

下面按函数说明。

### 5.1 `open_tcp(AddressFamily family)`

目标：

- 创建一个 TCP socket
- 默认就是 non-blocking
- 默认就是 close-on-exec

建议：

- 优先用 `::socket(native_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP)`
- 如果目标环境不支持在创建时附加 flag，再退化成 `socket + fcntl`

成功返回：

- `SocketHandle(fd)`

失败：

- 直接 `throw_errno(errno, "LinuxSocketApi::open_tcp()")`

### 5.2 `close(SocketHandle& socket)`

目标：

- 关闭 fd
- 成功后把 `socket` 置 invalid

约定：

- 这是少数返回 `error_code` 而不是抛异常的方法
- 如果传入本来就是 invalid socket，直接返回 success

建议：

- `::close(fd)` 成功后 `socket.reset()`
- 失败时返回 `make_error_code_from_errno(errno)`

### 5.3 `set_non_blocking(SocketHandle socket, bool enabled)`

目标：

- 用 `fcntl(F_GETFL/F_SETFL)` 设置或清除 `O_NONBLOCK`

失败：

- 抛异常

### 5.4 `set_reuse_address(SocketHandle socket, bool enabled)`

目标：

- `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ...)`

失败：

- 抛异常

### 5.5 `set_reuse_port(SocketHandle socket, bool enabled)`

目标：

- `setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, ...)`

注意：

- 这里要决定如果平台不支持 `SO_REUSEPORT`，是直接 `not_supported`，还是条件编译绕开
- 当前更建议明确报 `not_supported`，不要静默降级

### 5.6 `set_tcp_no_delay(SocketHandle socket, bool enabled)`

目标：

- `setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...)`

注意：

- 这里需要补 Linux TCP 头的 include

### 5.7 `bind(SocketHandle socket, const SocketAddress& local_address)`

目标：

- 调用 `::bind`

注意：

- `SocketAddress` 已经准备好了 `data()` 和 `size()`
- IPv4 / IPv6 不需要你自己再拆

失败：

- 抛异常

### 5.8 `listen(SocketHandle socket, int backlog)`

目标：

- 调用 `::listen`

建议：

- 决定 `backlog <= 0` 的行为
- 当前更建议直接报 `invalid_argument`

### 5.9 `accept(SocketHandle socket)`

目标：

- 接受新连接
- 新返回的 socket 也应该默认 non-blocking + close-on-exec
- 同时取到 peer address

建议：

- 优先 `::accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`
- 不行再退回 `accept + fcntl`

成功返回：

- 新 `SocketHandle`
- `SocketAddress::from_native(...)` 生成的 peer address

失败返回：

- `EAGAIN/EWOULDBLOCK` -> `errc::would_block`
- 其他错误 -> `make_error_code_from_errno(errno)`

注意：

- 这里是返回结果，不是抛异常

### 5.10 `connect(SocketHandle socket, const SocketAddress& remote_address)`

目标：

- 发起非阻塞连接

成功返回：

- `0` -> `{true, success}`

进行中返回：

- `EINPROGRESS` / `EALREADY` -> `{false, would_block}`

失败返回：

- 其他错误 -> `{false, 具体 error}`

注意：

- 这里也不是抛异常
- 这是后续 async connect 的关键对接点

### 5.11 `read_some(SocketHandle socket, MutableBuffer buffer)`

目标：

- 做一次尽力读取

建议：

- 用 `recv(fd, buffer.data, buffer.size, 0)` 即可

返回语义：

- `n > 0` -> 成功，`transferred = n`
- `n == 0` -> `end_of_file`
- `EAGAIN/EWOULDBLOCK` -> `would_block`
- 其他错误 -> 具体错误

### 5.12 `write_some(SocketHandle socket, ConstBuffer buffer)`

目标：

- 做一次尽力写入

建议：

- 优先 `send(..., MSG_NOSIGNAL)`

返回语义：

- `n >= 0` -> 成功，`transferred = n`
- `EAGAIN/EWOULDBLOCK` -> `would_block`
- `EPIPE` -> `broken_pipe`
- `ECONNRESET` -> `connection_reset`

### 5.13 `shutdown(SocketHandle socket, SocketShutdownMode mode)`

目标：

- 调用 `::shutdown(fd, SHUT_RD/SHUT_WR/SHUT_RDWR)`

失败：

- 抛异常

### 5.14 `local_address(SocketHandle socket)`

目标：

- `getsockname`

成功：

- `SocketAddress::from_native(...)`

失败：

- 抛异常

### 5.15 `peer_address(SocketHandle socket)`

目标：

- `getpeername`

成功：

- `SocketAddress::from_native(...)`

失败：

- 抛异常

## 6. async operation 交接点

如果交接对象不是“写 syscall 的人”，而是“写 async operation / backend 的人”，重点看这些文件：

- [mcqnet/include/mcqnet/net/socket_operations.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/net/socket_operations.h)
- [mcqnet/include/mcqnet/runtime/io_operation.h](/home/fank/workspace/mcqnet/mcqnet/include/mcqnet/runtime/io_operation.h)

### 6.1 公共模式

四个 operation 都遵循同一个模板：

1. `submit()` 里先同步尝试一次
2. 若结果是 `would_block`
3. 则通过 `submit_to_backend_after_would_block()` 接到 `IoBackend`
4. backend 完成时，把 richer 结果对象写回 operation 自身
5. `await_resume()` 返回结果对象；只有 cancel 走异常

现在这些步骤在代码里都已经直接标成了 TODO，不需要再根据文档自己找落点。

### 6.2 backend 完成时应该调用什么

不要优先裸调 `finish()`。

优先调用：

- `ConnectOperation::complete_connect()`
- `AcceptOperation::complete_accept()`
- `ReadOperation::complete_read()`
- `WriteOperation::complete_write()`

原因：

- `finish()` 只能保存 `result + error enum`
- 这些 higher-level helper 还能把 `SocketAddress`、`SocketHandle`、`native error` 这些 richer 状态保存在 operation 自身里

### 6.3 还没做的部分

这一层目前还是 skeleton，还没实现：

- backend 如何 downcast 到具体 operation 类型
- connect 完成后如何做 `SO_ERROR`/最终状态确认
- accept/read/write 在真实 backend 完成时如何回填结果
- `TcpStream` / `TcpListener` 上更高层的 async convenience API

但骨架已经够稳定，能把工作分给不同的人并行推进。

## 7. 错误映射要求

当前 `linux_socket_api.h` 已经有 `make_error_code_from_errno(int)`。

这意味着：

- 大部分 syscall 不要自己手写错误翻译
- 优先统一走 `make_error_code_from_errno(errno)`
- 需要抛异常的地方，直接 `throw_errno(errno, "...")`

目前已经预留了这些常见映射：

- `EACCES` / `EPERM` -> `permission_denied`
- `EADDRINUSE` -> `address_in_use`
- `EADDRNOTAVAIL` -> `address_not_available`
- `ECONNREFUSED` -> `connection_refused`
- `ECONNRESET` -> `connection_reset`
- `ECONNABORTED` -> `connection_aborted`
- `ENOTCONN` -> `not_connected`
- `EISCONN` -> `already_connected`
- `ENETUNREACH` -> `network_unreachable`
- `EHOSTUNREACH` -> `host_unreachable`
- `EPIPE` -> `broken_pipe`
- `EMSGSIZE` -> `message_too_large`
- `EAGAIN` / `EWOULDBLOCK` / `EINPROGRESS` / `EALREADY` -> `would_block`
- `ETIMEDOUT` -> `timed_out`
- `ENOMEM` / `ENOBUFS` -> `out_of_memory`

如果你发现映射不够，再补在这一个函数里，不要分散到每个 syscall 里各写一份。

## 8. 推荐实现顺序

不要乱填，按这个顺序最稳：

1. `open_tcp`
2. `close`
3. `set_non_blocking`
4. `set_reuse_address`
5. `set_reuse_port`
6. `set_tcp_no_delay`
7. `bind`
8. `listen`
9. `local_address`
10. `peer_address`
11. `accept`
12. `connect`
13. `read_some`
14. `write_some`
15. `shutdown`

如果是 async operation / backend 这条线，推荐顺序是：

1. `ConnectOperation`
2. `AcceptOperation`
3. `ReadOperation`
4. `WriteOperation`
5. 真正的 Linux completion backend

原因：

- 前半段先把 socket 生命周期和 listener 基础能力打稳
- 中段补地址获取，方便自测
- 后半段再补连接与收发

## 9. 实现时不要做的事

- 不要改 `SocketAddress` 的对外语义
- 不要把 `connect` 改成抛异常式接口
- 不要把 `read_some` 的 EOF 当成成功 `0` 返回
- 不要让 `accept` / `connect` 把 `would_block` 变成异常
- 不要把 runtime / backend 逻辑塞进 `linux_socket_api.h`
- 不要偷偷在析构里自动 close，当前对象所有权语义还没正式定稿
- 不要让 backend completion 绕开 `complete_connect()` / `complete_accept()` / `complete_read()` / `complete_write()` 直接只写 `finish()`

## 10. 自测建议

至少补这些测试：

- `TcpStream::open(AddressFamily::ipv4)` 可创建非阻塞 socket
- `TcpListener::open + bind + listen` 可在 loopback 地址监听
- `local_address()` 可返回正确端口
- 非阻塞 `accept_raw()` 在无连接时返回 `would_block`
- 非阻塞 `connect()` 到未监听端口时，返回 `would_block` 或后续连接错误语义符合预期
- `read_some()` 在对端关闭时返回 `end_of_file`
- `write_some()` 在连接断开后能映射到 `broken_pipe` 或 `connection_reset`
- IPv6 loopback 路径可工作

operation / backend 这一层至少补这些测试：

- `ConnectOperation` / `AcceptOperation` / `ReadOperation` / `WriteOperation` 可构造并保留目标对象状态
- 直接同步成功时，`await_resume()` 返回正确结果结构
- `would_block` 时会进入 `IoBackend::submit()`
- cancel 时 waiter 以 `operation_aborted` 恢复
- backend 完成路径通过 `complete_*()` 回填 richer 结果后，`await_resume()` 能看到完整状态

## 11. 完成标准

这层完成的标准不是“已经异步化”，而是下面这些：

- `linux_socket_api.h` 里的 `TODO(socket)` 全部消掉
- `TcpStream` / `TcpListener` 的对应 public 方法可以真实工作
- IPv4 / IPv6 都能通
- 非阻塞语义正确
- `would_block` / `EOF` / `connect in progress` 都按现有结果类型表达
- 不破坏当前 runtime / IO backend 的设计边界

做完这一步之后，后面的人就可以基于这层去接：

- async `ConnectOperation`
- async `AcceptOperation`
- async `ReadOperation` / `WriteOperation`
- Linux completion backend，例如 `io_uring`
