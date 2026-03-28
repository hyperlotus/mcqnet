#pragma once

// TcpListener 的第一阶段骨架。
// 当前在“监听 socket + runtime 绑定”之上，再补 Linux listener 操作入口：
// - open/bind/listen/accept 的 syscall 细节统一下沉到 linux_socket_api.h
// - 这里保留稳定对象语义和更高层方法签名
// - 未来 accept awaitable 可以直接复用 accept_raw() 的返回形状

#include <mcqnet/config/platform.h>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/net/socket_address.h>
#include <mcqnet/net/socket_handle.h>
#include <mcqnet/net/socket_result.h>
#include <mcqnet/runtime/handle.h>

#if MCQNET_PLATFORM_LINUX
#include <mcqnet/net/linux_socket_api.h>
#endif

namespace mcqnet::net {

// TcpListener 和 TcpStream 一样保持薄包装定位：
// - 负责 listener socket 的对象持有与 runtime 绑定
// - 具体 bind/listen/accept syscall 语义都下沉到 LinuxSocketApi
// - async accept operation 直接复用这里的同步/非阻塞协议
class TcpListener {
public:
    TcpListener() noexcept = default;

    explicit TcpListener(SocketHandle socket, runtime::Handle runtime_handle = {}) noexcept
        : socket_(socket)
        , runtime_handle_(runtime_handle) { }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&& other) noexcept
        : socket_(other.socket_)
        , runtime_handle_(other.runtime_handle_) {
        other.socket_.reset();
        other.runtime_handle_ = {};
    }

    TcpListener& operator=(TcpListener&& other) noexcept {
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

    // runtime handle 只影响默认 operation 绑定，不改变 socket 自身行为。
    inline void set_runtime_handle(runtime::Handle runtime_handle) noexcept { runtime_handle_ = runtime_handle; }

    MCQNET_NODISCARD
    static inline TcpListener open(AddressFamily family, runtime::Handle runtime_handle = {}) {
#if MCQNET_PLATFORM_LINUX
        return TcpListener { linux_detail::LinuxSocketApi::open_tcp(family), runtime_handle };
#else
        (void)family;
        (void)runtime_handle;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpListener::open() is only planned for Linux");
#endif
    }

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
            core::error_code { core::errc::not_supported }, "TcpListener::set_non_blocking() is only planned for Linux");
#endif
    }

    inline void set_reuse_address(bool enabled) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::set_reuse_address(socket_, enabled);
#else
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpListener::set_reuse_address() is only planned for Linux");
#endif
    }

    inline void set_reuse_port(bool enabled) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::set_reuse_port(socket_, enabled);
#else
        (void)enabled;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpListener::set_reuse_port() is only planned for Linux");
#endif
    }

    inline void bind(const SocketAddress& local_address) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::bind(socket_, local_address);
#else
        (void)local_address;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpListener::bind() is only planned for Linux");
#endif
    }

    inline void listen(int backlog = 128) {
#if MCQNET_PLATFORM_LINUX
        linux_detail::LinuxSocketApi::listen(socket_, backlog);
#else
        (void)backlog;
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpListener::listen() is only planned for Linux");
#endif
    }

    // accept_raw() 暂时直接暴露 native 结果：
    // - success 时给出新 socket + peer address
    // - would_block 时由返回值表达，而不是抛异常
    // 这样未来 async accept operation 只需要把这个结果接到 backend completion 即可。
    MCQNET_NODISCARD
    inline AcceptResult accept_raw() const noexcept {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::accept(socket_);
#else
        return AcceptResult { { }, { }, core::error_code { core::errc::not_supported } };
#endif
    }

    MCQNET_NODISCARD
    inline SocketAddress local_address() const {
#if MCQNET_PLATFORM_LINUX
        return linux_detail::LinuxSocketApi::local_address(socket_);
#else
        core::throw_net_error(
            core::error_code { core::errc::not_supported }, "TcpListener::local_address() is only planned for Linux");
#endif
    }

    // release_socket()/reset() 的语义与 TcpStream 保持一致：只做所有权移动，不隐式 close。
    MCQNET_NODISCARD
    inline SocketHandle release_socket() noexcept {
        const SocketHandle socket = socket_;
        socket_.reset();
        return socket;
    }

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

using net::TcpListener;

} // namespace mcqnet
