#pragma once

// TcpStream 的第一阶段骨架。
// 当前在原有“socket handle + runtime 绑定”之上，再补一层同步/非阻塞 socket 入口：
// - 具体 Linux syscall 全放到 linux_socket_api.h
// - 这里负责承载面向用户的 stream 对象方法
// - 未来 async connect/read/write operation 直接复用这些 public 语义和返回类型

#include <mcqnet/config/platform.h>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/net/buffer.h>
#include <mcqnet/net/socket_address.h>
#include <mcqnet/net/socket_handle.h>
#include <mcqnet/net/socket_result.h>
#include <mcqnet/runtime/handle.h>

#if MCQNET_PLATFORM_LINUX
#include <mcqnet/net/linux_socket_api.h>
#endif

namespace mcqnet::net {

// TcpStream 目前是一个薄包装：
// - 持有 native socket handle
// - 可选绑定 runtime handle，供 async operation 默认继承
// - 同步/非阻塞语义全部委托到 LinuxSocketApi
//
// 交接时不要把平台 syscall 细节重新塞回这个类；这里只维护对象语义。
class TcpStream {
public:
    TcpStream() noexcept = default;

    explicit TcpStream(SocketHandle socket, runtime::Handle runtime_handle = {}) noexcept
        : socket_(socket)
        , runtime_handle_(runtime_handle) { }

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    TcpStream(TcpStream&& other) noexcept
        : socket_(other.socket_)
        , runtime_handle_(other.runtime_handle_) {
        other.socket_.reset();
        other.runtime_handle_ = {};
    }

    TcpStream& operator=(TcpStream&& other) noexcept {
        if ( this != &other ) {
            socket_ = other.socket_;
            runtime_handle_ = other.runtime_handle_;
            other.socket_.reset();
            other.runtime_handle_ = {};
        }
        return *this;
    }

    MCQNET_NODISCARD
    inline bool valid() const noexcept { return socket_.valid(); }

    MCQNET_NODISCARD
    inline explicit operator bool() const noexcept { return valid(); }

    MCQNET_NODISCARD
    inline const SocketHandle& socket() const noexcept { return socket_; }

    MCQNET_NODISCARD
    inline SocketHandle::native_handle_type native_handle() const noexcept { return socket_.native_handle(); }

    MCQNET_NODISCARD
    inline runtime::Handle runtime_handle() const noexcept { return runtime_handle_; }

    // runtime 绑定不是 socket 原生属性，只是给 operation 提供默认 runtime。
    inline void set_runtime_handle(runtime::Handle runtime_handle) noexcept { runtime_handle_ = runtime_handle; }

    // 创建一个尚未 connect 的 TCP stream。
    // 当前 Linux-only 预留给原始 socket() 封装；Windows 方案后续单独补。
    MCQNET_NODISCARD
    static inline TcpStream open(AddressFamily family, runtime::Handle runtime_handle = {}) {
#if MCQNET_PLATFORM_LINUX
        return TcpStream { linux_detail::LinuxSocketApi::open_tcp(family), runtime_handle };
#else
        (void)family;
        (void)runtime_handle;
        core::throw_net_error(core::error_code { core::errc::not_supported }, "TcpStream::open() is only planned for Linux");
#endif
    }

    // 显式 close；当前析构仍不自动 close，避免在 ownership 语义未定前引入破坏性行为。
    // 交接实现时，优先在 LinuxSocketApi::close() 填 syscall，这里保持薄包装即可。
    MCQNET_NODISCARD
    inline core::error_code close() noexcept {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::close(socket_);
#else
        return core::error_code { core::errc::not_supported };
#endif
    }

    inline void set_non_blocking(bool enabled) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::set_non_blocking(socket_, enabled);
#else
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpStream::set_non_blocking() is only planned for Linux");
#endif
    }

    inline void set_tcp_no_delay(bool enabled) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::set_tcp_no_delay(socket_, enabled);
#else
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpStream::set_tcp_no_delay() is only planned for Linux");
#endif
    }

    // connect 返回结果而不是直接抛异常，是为了保留 non-blocking connect 的 in-progress 语义。
    // 未来 async connect operation 可以直接把 in_progress 接到 backend 可写事件上。
    MCQNET_NODISCARD
    inline ConnectResult connect(const SocketAddress& remote_address) noexcept {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::connect(socket_, remote_address);
#else
        (void)remote_address;
        return ConnectResult { false, core::error_code { core::errc::not_supported } };
#endif
    }

    // read/write 也先返回显式结果，便于区分 would_block / EOF / hard error。
    // 后续 awaitable 可以直接复用这个语义，而不是重新定义一套状态机。
    MCQNET_NODISCARD
    inline SocketIoResult read_some(MutableBuffer buffer) noexcept {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::read_some(socket_, buffer);
#else
        (void)buffer;
        return SocketIoResult { 0, core::error_code { core::errc::not_supported } };
#endif
    }

    MCQNET_NODISCARD
    inline SocketIoResult write_some(ConstBuffer buffer) noexcept {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::write_some(socket_, buffer);
#else
        (void)buffer;
        return SocketIoResult { 0, core::error_code { core::errc::not_supported } };
#endif
    }

    inline void shutdown(SocketShutdownMode mode) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::shutdown(socket_, mode);
#else
        (void)mode;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpStream::shutdown() is only planned for Linux");
#endif
    }

    MCQNET_NODISCARD
    inline SocketAddress local_address() const {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::local_address(socket_);
#else
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpStream::local_address() is only planned for Linux");
#endif
    }

    MCQNET_NODISCARD
    inline SocketAddress peer_address() const {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::peer_address(socket_);
#else
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpStream::peer_address() is only planned for Linux");
#endif
    }

    // release_socket() 会转移 native handle 所有权，并把当前对象置空。
    // 后续若把 accepted socket 组装成 TcpStream，这个入口可直接复用。
    MCQNET_NODISCARD
    inline SocketHandle release_socket() noexcept {
        const SocketHandle socket = socket_;
        socket_.reset();
        return socket;
    }

    // reset() 只改本地持有状态，不隐式 close 旧 socket。
    // 交接实现时若要覆盖已有对象，必须先由调用方显式处理旧 handle 生命周期。
    inline void reset(SocketHandle socket = {}, runtime::Handle runtime_handle = {}) noexcept {
        socket_ = socket;
        runtime_handle_ = runtime_handle;
    }

private:
    SocketHandle socket_ { };
    runtime::Handle runtime_handle_ { };
};

} // namespace mcqnet::net

namespace mcqnet {

using net::TcpStream;

} // namespace mcqnet
