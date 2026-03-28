#pragma once

// Socket 这一层的统一结果类型。
// 设计目的：
// - 把 would_block / EOF / connect-in-progress 这些非异常控制流显式化
// - 让 Linux syscall 封装、TcpStream/TcpListener 公共方法、未来 async operation
//   可以共用同一批返回结构，避免后面再拆协议

#include <cstddef>
#include <cstdint>
#include <mcqnet/core/error.h>
#include <mcqnet/detail/macro.h>
#include <mcqnet/net/socket_address.h>
#include <mcqnet/net/socket_handle.h>

namespace mcqnet::net {

// 和 SocketAddress 一样，这个 family 枚举会同时被 public wrapper、syscall 层、
// 以及后续 backend 适配层复用；不要为单个平台单独改枚举值。
enum class SocketShutdownMode : std::uint8_t {
    receive = 0,
    send = 1,
    both = 2
};

// 统一表达一次 read/write 风格操作的结果。
// 约定：
// - success(): 表示 syscall/backend 结束且没有错误
// - would_block(): 表示需要切到异步等待，不应当被包装成异常
// - eof(): 只用于 read 路径；write 不应主动返回 EOF
struct SocketIoResult {
    std::size_t transferred { 0 };
    core::error_code error { };

    MCQNET_NODISCARD
    constexpr bool success() const noexcept { return !error; }

    MCQNET_NODISCARD
    constexpr bool would_block() const noexcept { return error.value == core::errc::would_block; }

    MCQNET_NODISCARD
    constexpr bool eof() const noexcept { return error.value == core::errc::end_of_file; }
};

// 非阻塞 connect 不能只用成功/失败二值表达，所以单独保留 completed 位。
// 约定：
// - completed=true && !error: 已连上
// - completed=false && would_block: 还在进行中，交给 operation/backend 等待可写完成
// - completed=false && 其它 error: connect 失败
struct ConnectResult {
    bool completed { false };
    core::error_code error { };

    MCQNET_NODISCARD
    constexpr bool success() const noexcept { return completed && !error; }

    MCQNET_NODISCARD
    constexpr bool in_progress() const noexcept { return !completed && error.value == core::errc::would_block; }
};

// accept 成功时必须同时带回新 socket 和 peer address。
// 若 backend 以后直接产出 completion，也应最终收束成这个结构，避免上层协议分叉。
struct AcceptResult {
    SocketHandle socket { };
    SocketAddress peer_address { };
    core::error_code error { };

    MCQNET_NODISCARD
    constexpr bool success() const noexcept { return socket.valid() && !error; }

    MCQNET_NODISCARD
    constexpr bool would_block() const noexcept { return error.value == core::errc::would_block; }
};

} // namespace mcqnet::net

namespace mcqnet {

using net::AcceptResult;
using net::ConnectResult;
using net::SocketIoResult;
using net::SocketShutdownMode;

} // namespace mcqnet
