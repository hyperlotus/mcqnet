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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mcqnet::net::linux_detail {

class LinuxSocketApi {
public:
    MCQNET_NODISCARD
    static inline core::error_code error_code_from_errno(int native_error) noexcept {
        return make_error_code_from_errno(native_error);
    }

    MCQNET_NODISCARD
    static inline SocketHandle open_tcp(AddressFamily family) {
#if MCQNET_PLATFORM_LINUX
        const int native_family = native_address_family(family);
        const int native_socket = ::socket(native_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if ( native_socket < 0 ) {
            throw_errno(errno, "LinuxSocketApi::open_tcp()");
        }
        return SocketHandle { native_socket };
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

        if ( ::close(socket.native_handle()) == 0 ) {
            socket.reset();
            return { };
        }
        return make_error_code_from_errno(errno);
#else
        (void)socket;
        return core::error_code { core::errc::not_supported };
#endif
    }

    static inline void set_non_blocking(SocketHandle socket, bool enabled) {
#if MCQNET_PLATFORM_LINUX
        require_valid_socket(socket, "LinuxSocketApi::set_non_blocking()");
        const int native_socket = socket.native_handle();
        const int flags = ::fcntl(native_socket, F_GETFL, 0);
        if ( flags < 0 ) {
            throw_errno(errno, "LinuxSocketApi::set_non_blocking()");
        }

        const int next_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        if ( next_flags == flags ) {
            return;
        }
        if ( ::fcntl(native_socket, F_SETFL, next_flags) < 0 ) {
            throw_errno(errno, "LinuxSocketApi::set_non_blocking()");
        }
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
        set_socket_option_int(socket, SOL_SOCKET, SO_REUSEADDR, enabled, "LinuxSocketApi::set_reuse_address()");
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
#ifdef SO_REUSEPORT
        set_socket_option_int(socket, SOL_SOCKET, SO_REUSEPORT, enabled, "LinuxSocketApi::set_reuse_port()");
#else
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported },
            "LinuxSocketApi::set_reuse_port() is not supported on this platform");
#endif
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
        set_socket_option_int(socket, IPPROTO_TCP, TCP_NODELAY, enabled, "LinuxSocketApi::set_tcp_no_delay()");
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

        if ( ::bind(socket.native_handle(), local_address.data(), local_address.size()) < 0 ) {
            throw_errno(errno, "LinuxSocketApi::bind()");
        }
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
        if ( backlog <= 0 ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument },
                "LinuxSocketApi::listen() requires a positive backlog");
        }

        if ( ::listen(socket.native_handle(), backlog) < 0 ) {
            throw_errno(errno, "LinuxSocketApi::listen()");
        }
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

        sockaddr_storage storage { };
        socklen_t storage_length = static_cast<socklen_t>(sizeof(storage));

        for ( ;; ) {
            const int accepted_socket = ::accept4(
                socket.native_handle(),
                reinterpret_cast<sockaddr*>(&storage),
                &storage_length,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
            if ( accepted_socket >= 0 ) {
                return AcceptResult {
                    SocketHandle { accepted_socket },
                    SocketAddress::from_native(reinterpret_cast<const sockaddr*>(&storage), storage_length),
                    { }
                };
            }

            if ( errno == EINTR ) {
                continue;
            }
            if ( errno == EAGAIN
#if EAGAIN != EWOULDBLOCK
                 || errno == EWOULDBLOCK
#endif
            ) {
                return AcceptResult { { }, { }, core::error_code { core::errc::would_block, static_cast<std::uint32_t>(errno) } };
            }
            return AcceptResult { { }, { }, make_error_code_from_errno(errno) };
        }
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

        if ( ::connect(socket.native_handle(), remote_address.data(), remote_address.size()) == 0 ) {
            return ConnectResult { true, { } };
        }

        if ( errno == EISCONN ) {
            return ConnectResult { true, { } };
        }

        const core::error_code error = make_error_code_from_errno(errno);
        if ( error.value == core::errc::would_block ) {
            return ConnectResult { false, error };
        }
        return ConnectResult { false, error };
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

        if ( buffer.empty() ) {
            return SocketIoResult { 0, { } };
        }

        for ( ;; ) {
            const ssize_t read_size = ::recv(socket.native_handle(), buffer.data, buffer.size, 0);
            if ( read_size > 0 ) {
                return SocketIoResult { static_cast<std::size_t>(read_size), { } };
            }
            if ( read_size == 0 ) {
                return SocketIoResult { 0, core::error_code { core::errc::end_of_file } };
            }
            if ( errno == EINTR ) {
                continue;
            }
            return SocketIoResult { 0, make_error_code_from_errno(errno) };
        }
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

        if ( buffer.empty() ) {
            return SocketIoResult { 0, { } };
        }

        for ( ;; ) {
            const ssize_t written_size =
                ::send(socket.native_handle(), buffer.data, buffer.size, MSG_NOSIGNAL);
            if ( written_size >= 0 ) {
                return SocketIoResult { static_cast<std::size_t>(written_size), { } };
            }
            if ( errno == EINTR ) {
                continue;
            }
            return SocketIoResult { 0, make_error_code_from_errno(errno) };
        }
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
        if ( ::shutdown(socket.native_handle(), native_mode) < 0 ) {
            throw_errno(errno, "LinuxSocketApi::shutdown()");
        }
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
        sockaddr_storage storage { };
        socklen_t storage_length = static_cast<socklen_t>(sizeof(storage));
        if ( ::getsockname(socket.native_handle(), reinterpret_cast<sockaddr*>(&storage), &storage_length) < 0 ) {
            throw_errno(errno, "LinuxSocketApi::local_address()");
        }
        return SocketAddress::from_native(reinterpret_cast<const sockaddr*>(&storage), storage_length);
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
        sockaddr_storage storage { };
        socklen_t storage_length = static_cast<socklen_t>(sizeof(storage));
        if ( ::getpeername(socket.native_handle(), reinterpret_cast<sockaddr*>(&storage), &storage_length) < 0 ) {
            throw_errno(errno, "LinuxSocketApi::peer_address()");
        }
        return SocketAddress::from_native(reinterpret_cast<const sockaddr*>(&storage), storage_length);
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

#if MCQNET_PLATFORM_LINUX
    static inline void set_socket_option_int(
        SocketHandle socket, int level, int option_name, bool enabled, std::string_view what) {
        const int value = enabled ? 1 : 0;
        if ( ::setsockopt(socket.native_handle(), level, option_name, &value, sizeof(value)) < 0 ) {
            throw_errno(errno, what);
        }
    }
#endif

    MCQNET_NODISCARD
    static inline core::error_code make_error_code_from_errno(int native_error) noexcept {
#if MCQNET_PLATFORM_LINUX
        switch ( native_error ) {
            case 0: return { };
            case EINTR: return core::error_code { core::errc::operation_aborted, static_cast<std::uint32_t>(native_error) };
            case ECANCELED:
                return core::error_code { core::errc::operation_aborted, static_cast<std::uint32_t>(native_error) };
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
