#pragma once

// Linux socket syscall 封装骨架。
// 这层只做三件事：
// - 承接原始 syscall / sockopt / errno 映射
// - 不碰 runtime / awaitable / backend 提交逻辑
// - 给上层 TcpStream/TcpListener 提供“可直接补函数体”的稳定入口
//
// 交接建议：
// 1. 优先把本文件里的 TODO(socket) 补完，确保同步/非阻塞 syscall 语义稳定
// 2. 之后再在更高层接 Async Operation / io_uring backend
// 3. 不要在这里引入协程状态机；这里只负责 native socket 语义

#include <mcqnet/config/platform.h>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/net/buffer.h>
#include <mcqnet/net/socket_result.h>
#include <string>
#include <string_view>

#if MCQNET_PLATFORM_LINUX
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace mcqnet::net::linux_detail {

class LinuxSocketApi {
public:
    MCQNET_NODISCARD
    static inline SocketHandle open_tcp(AddressFamily family) {
#if MCQNET_PLATFORM_LINUX
        const int native_family = native_address_family(family);
        (void)native_family;

        // TODO(socket): 调用 ::socket(native_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP)。
        // TODO(socket): 若创建时不能带 NONBLOCK/CLOEXEC，则在成功后立刻补 fcntl()。
        // TODO(socket): 失败时使用 throw_errno(errno, "LinuxSocketApi::open_tcp()")。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::open_tcp() is a scaffold; fill the syscall body");
#else
        (void)family;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    static inline core::error_code close(SocketHandle& socket) noexcept {
#if MCQNET_PLATFORM_LINUX
        if ( !socket.valid() ) {
            return { };
        }

        // TODO(socket): 调用 ::close(fd)。
        // TODO(socket): 成功后执行 socket.reset()。
        // TODO(socket): 失败时返回 make_error_code_from_errno(errno)，不要抛异常。
        return core::error_code { core::errc::not_supported };
#else
        (void)socket;
        return core::error_code { core::errc::not_supported };
#endif
    }

    static inline void set_non_blocking(SocketHandle socket, bool enabled) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::set_non_blocking()");
        (void)enabled;

        // TODO(socket): 用 ::fcntl(fd, F_GETFL/F_SETFL) 增删 O_NONBLOCK。
        // TODO(socket): 失败时使用 throw_errno(errno, "LinuxSocketApi::set_non_blocking()")。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::set_non_blocking() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    static inline void set_reuse_address(SocketHandle socket, bool enabled) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::set_reuse_address()");
        (void)enabled;

        // TODO(socket): 用 ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ...)。
        // TODO(socket): 失败时使用 throw_errno(errno, "LinuxSocketApi::set_reuse_address()")。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::set_reuse_address() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    static inline void set_reuse_port(SocketHandle socket, bool enabled) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::set_reuse_port()");
        (void)enabled;

        // TODO(socket): 用 ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, ...)。
        // TODO(socket): 若目标平台/内核不支持，需要在这里决定是直接报 not_supported 还是条件编译降级。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::set_reuse_port() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    static inline void set_tcp_no_delay(SocketHandle socket, bool enabled) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::set_tcp_no_delay()");
        (void)enabled;

        // TODO(socket): 用 ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...)。
        // TODO(socket): 失败时使用 throw_errno(errno, "LinuxSocketApi::set_tcp_no_delay()")。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::set_tcp_no_delay() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    static inline void bind(SocketHandle socket, const SocketAddress& local_address) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::bind()");
        if ( !local_address.valid() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument },
                "LinuxSocketApi::bind() requires a valid local address");
        }

        // TODO(socket): 调用 ::bind(fd, local_address.data(), local_address.size())。
        // TODO(socket): 失败时使用 throw_errno(errno, "LinuxSocketApi::bind()")。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::bind() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)local_address;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    static inline void listen(SocketHandle socket, int backlog) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::listen()");
        (void)backlog;

        // TODO(socket): 调用 ::listen(fd, backlog)。
        // TODO(socket): 在这里决定 backlog <= 0 时是否主动修正或直接报 invalid_argument。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::listen() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)backlog;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    MCQNET_NODISCARD
    static inline AcceptResult accept(SocketHandle socket) noexcept {
#if MCQNET_PLATFORM_LINUX
        if ( !socket.valid() ) {
            return AcceptResult { { }, { }, core::error_code { core::errc::invalid_argument } };
        }

        // TODO(socket): 优先使用 ::accept4(fd, ..., SOCK_NONBLOCK | SOCK_CLOEXEC)。
        // TODO(socket): 若 accept4 不可用，再 accept + fcntl 补 flag。
        // TODO(socket): 成功时返回 {SocketHandle(new_fd), peer_address, {}}。
        // TODO(socket): EAGAIN/EWOULDBLOCK 返回 would_block；其它 errno 走 make_error_code_from_errno(errno)。
        return AcceptResult { { }, { }, core::error_code { core::errc::not_supported } };
#else
        (void)socket;
        return AcceptResult { { }, { }, core::error_code { core::errc::not_supported } };
#endif
    }

    MCQNET_NODISCARD
    static inline ConnectResult connect(SocketHandle socket, const SocketAddress& remote_address) noexcept {
#if MCQNET_PLATFORM_LINUX
        if ( !socket.valid() || !remote_address.valid() ) {
            return ConnectResult { false, core::error_code { core::errc::invalid_argument } };
        }

        // TODO(socket): 调用 ::connect(fd, remote_address.data(), remote_address.size())。
        // TODO(socket): 返回 0 => {true, {}}。
        // TODO(socket): EINPROGRESS/EALREADY => {false, would_block}，交给上层做可写等待。
        // TODO(socket): 其它 errno => {false, make_error_code_from_errno(errno)}。
        return ConnectResult { false, core::error_code { core::errc::not_supported } };
#else
        (void)socket;
        (void)remote_address;
        return ConnectResult { false, core::error_code { core::errc::not_supported } };
#endif
    }

    MCQNET_NODISCARD
    static inline SocketIoResult read_some(SocketHandle socket, MutableBuffer buffer) noexcept {
#if MCQNET_PLATFORM_LINUX
        if ( !socket.valid() || (!buffer && !buffer.empty()) ) {
            return SocketIoResult { 0, core::error_code { core::errc::invalid_argument } };
        }

        // TODO(socket): 调用 ::recv(fd, buffer.data, buffer.size, 0) 或 ::read(fd, ...)。
        // TODO(socket): 返回 0 时映射为 EOF：{0, end_of_file}。
        // TODO(socket): EAGAIN/EWOULDBLOCK => would_block；其它 errno 走 make_error_code_from_errno(errno)。
        return SocketIoResult { 0, core::error_code { core::errc::not_supported } };
#else
        (void)socket;
        (void)buffer;
        return SocketIoResult { 0, core::error_code { core::errc::not_supported } };
#endif
    }

    MCQNET_NODISCARD
    static inline SocketIoResult write_some(SocketHandle socket, ConstBuffer buffer) noexcept {
#if MCQNET_PLATFORM_LINUX
        if ( !socket.valid() || (!buffer && !buffer.empty()) ) {
            return SocketIoResult { 0, core::error_code { core::errc::invalid_argument } };
        }

        // TODO(socket): 调用 ::send(fd, buffer.data, buffer.size, MSG_NOSIGNAL) 或等价方案。
        // TODO(socket): 成功时返回实际写入字节数。
        // TODO(socket): EAGAIN/EWOULDBLOCK => would_block；EPIPE/ECONNRESET 等走 make_error_code_from_errno(errno)。
        return SocketIoResult { 0, core::error_code { core::errc::not_supported } };
#else
        (void)socket;
        (void)buffer;
        return SocketIoResult { 0, core::error_code { core::errc::not_supported } };
#endif
    }

    static inline void shutdown(SocketHandle socket, SocketShutdownMode mode) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::shutdown()");
        const int native_mode = native_shutdown_mode(mode);
        (void)native_mode;

        // TODO(socket): 调用 ::shutdown(fd, native_mode)。
        // TODO(socket): 失败时使用 throw_errno(errno, "LinuxSocketApi::shutdown()")。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::shutdown() is a scaffold; fill the syscall body");
#else
        (void)socket;
        (void)mode;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    MCQNET_NODISCARD
    static inline SocketAddress local_address(SocketHandle socket) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::local_address()");

        // TODO(socket): 调用 ::getsockname(fd, storage, &len)。
        // TODO(socket): 用 SocketAddress::from_native(...) 统一转成库内地址对象。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::local_address() is a scaffold; fill the syscall body");
#else
        (void)socket;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

    MCQNET_NODISCARD
    static inline SocketAddress peer_address(SocketHandle socket) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::peer_address()");

        // TODO(socket): 调用 ::getpeername(fd, storage, &len)。
        // TODO(socket): 用 SocketAddress::from_native(...) 统一转成库内地址对象。
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::peer_address() is a scaffold; fill the syscall body");
#else
        (void)socket;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi is only available on Linux");
#endif
    }

private:
#if MCQNET_PLATFORM_LINUX
    MCQNET_NODISCARD
    static inline int native_address_family(AddressFamily family) {
        switch ( family ) {
            case AddressFamily::ipv4: return AF_INET;
            case AddressFamily::ipv6: return AF_INET6;
            default:
                core::throw_net_error(
                    core::error_code { core::errc::invalid_argument },
                    "LinuxSocketApi requires an IPv4 or IPv6 address family");
        }
    }

    MCQNET_NODISCARD
    static inline int native_shutdown_mode(SocketShutdownMode mode) {
        switch ( mode ) {
            case SocketShutdownMode::receive: return SHUT_RD;
            case SocketShutdownMode::send: return SHUT_WR;
            case SocketShutdownMode::both: return SHUT_RDWR;
            default:
                core::throw_net_error(
                    core::error_code { core::errc::invalid_argument },
                    "LinuxSocketApi received an invalid shutdown mode");
        }
    }
#endif

    static inline void require_valid_socket(SocketHandle socket, std::string_view what) {
        if ( !socket.valid() ) {
            core::throw_net_error(core::error_code { core::errc::invalid_argument }, std::string(what));
        }
    }

    MCQNET_NODISCARD
    static inline core::error_code make_error_code_from_errno(int native_error) noexcept {
#if MCQNET_PLATFORM_LINUX
        switch ( native_error ) {
            case 0: return { };
            case EACCES:
            case EPERM: return core::error_code { core::errc::permission_denied, static_cast<std::uint32_t>(native_error) };
            case EADDRINUSE: return core::error_code { core::errc::address_in_use, static_cast<std::uint32_t>(native_error) };
            case EADDRNOTAVAIL:
                return core::error_code { core::errc::address_not_available, static_cast<std::uint32_t>(native_error) };
            case ECONNREFUSED:
                return core::error_code { core::errc::connection_refused, static_cast<std::uint32_t>(native_error) };
            case ECONNRESET:
                return core::error_code { core::errc::connection_reset, static_cast<std::uint32_t>(native_error) };
            case ECONNABORTED:
                return core::error_code { core::errc::connection_aborted, static_cast<std::uint32_t>(native_error) };
            case ENOTCONN: return core::error_code { core::errc::not_connected, static_cast<std::uint32_t>(native_error) };
            case EISCONN: return core::error_code { core::errc::already_connected, static_cast<std::uint32_t>(native_error) };
            case ENETUNREACH:
                return core::error_code { core::errc::network_unreachable, static_cast<std::uint32_t>(native_error) };
            case EHOSTUNREACH:
                return core::error_code { core::errc::host_unreachable, static_cast<std::uint32_t>(native_error) };
            case EPIPE: return core::error_code { core::errc::broken_pipe, static_cast<std::uint32_t>(native_error) };
            case EMSGSIZE:
                return core::error_code { core::errc::message_too_large, static_cast<std::uint32_t>(native_error) };
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
            case EINPROGRESS:
            case EALREADY:
                return core::error_code { core::errc::would_block, static_cast<std::uint32_t>(native_error) };
            case ETIMEDOUT: return core::error_code { core::errc::timed_out, static_cast<std::uint32_t>(native_error) };
            case ENOMEM:
            case ENOBUFS: return core::error_code { core::errc::out_of_memory, static_cast<std::uint32_t>(native_error) };
            case EAFNOSUPPORT:
            case EPROTONOSUPPORT:
            case EOPNOTSUPP:
#if EOPNOTSUPP != ENOTSUP
            case ENOTSUP: return core::error_code { core::errc::not_supported, static_cast<std::uint32_t>(native_error) };
#else
                return core::error_code { core::errc::not_supported, static_cast<std::uint32_t>(native_error) };
#endif
            case ENOENT: return core::error_code { core::errc::not_found, static_cast<std::uint32_t>(native_error) };
            case EEXIST: return core::error_code { core::errc::already_exists, static_cast<std::uint32_t>(native_error) };
            default: return core::error_code { core::errc::io_error, static_cast<std::uint32_t>(native_error) };
        }
#else
        (void)native_error;
        return core::error_code { core::errc::not_supported };
#endif
    }

    [[noreturn]] static inline void throw_errno(int native_error, std::string_view what) {
        core::throw_net_error(make_error_code_from_errno(native_error), std::string(what));
    }
};

} // namespace mcqnet::net::linux_detail
