#pragma once

// SocketAddress：
// - 统一承载 IPv4 / IPv6 地址
// - 保存 native sockaddr_storage + length，供未来 connect/bind/accept 直接复用
// - 当前先支持 literal 构造与解析，不做 DNS 解析

#include <array>
#include <cstdint>
#include <cstring>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/macro.h>
#include <optional>
#include <string>
#include <string_view>

#if MCQNET_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace mcqnet::net {

// 这个枚举是逻辑 family，不等同于平台原生 AF_* 常量。
// syscall 边界统一在 linux_socket_api.h 内做映射，避免 public 层被平台值污染。
enum class AddressFamily : std::uint8_t {
    unspecified = 0,
    ipv4 = 1,
    ipv6 = 2
};

// SocketAddress 持有完整 sockaddr_storage 副本。
// 这样做的目的有两个：
// - 上层对象可以值语义传递，不依赖外部 sockaddr 生命周期
// - bind/connect/accept/getsockname 之间可直接复用 native 内存布局
class SocketAddress {
public:
#if MCQNET_PLATFORM_WINDOWS
    using native_size_type = int;
#else
    using native_size_type = socklen_t;
#endif

    SocketAddress() noexcept = default;

    MCQNET_NODISCARD
    static inline SocketAddress ipv4(
        std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) noexcept {
        sockaddr_in address { };
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        auto* bytes = reinterpret_cast<unsigned char*>(&address.sin_addr);
        bytes[0] = a;
        bytes[1] = b;
        bytes[2] = c;
        bytes[3] = d;
        return from_ipv4_native(address);
    }

    MCQNET_NODISCARD
    static inline SocketAddress ipv4_any(std::uint16_t port = 0) noexcept { return ipv4(0, 0, 0, 0, port); }

    MCQNET_NODISCARD
    static inline SocketAddress ipv4_loopback(std::uint16_t port = 0) noexcept { return ipv4(127, 0, 0, 1, port); }

    MCQNET_NODISCARD
    static inline SocketAddress ipv6(
        const std::array<std::uint8_t, 16>& bytes, std::uint16_t port, std::uint32_t scope_id = 0) noexcept {
        sockaddr_in6 address { };
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        address.sin6_scope_id = scope_id;
        std::memcpy(&address.sin6_addr, bytes.data(), bytes.size());
        return from_ipv6_native(address);
    }

    MCQNET_NODISCARD
    static inline SocketAddress ipv6_any(std::uint16_t port = 0) noexcept { return ipv6({ }, port); }

    MCQNET_NODISCARD
    static inline SocketAddress ipv6_loopback(std::uint16_t port = 0, std::uint32_t scope_id = 0) noexcept {
        std::array<std::uint8_t, 16> bytes { };
        bytes[15] = 1;
        return ipv6(bytes, port, scope_id);
    }

    // 这里只接收 IP literal；不做 DNS 解析，也不接受 "host:port" 整串输入。
    // IPv6 可传裸 literal，也可传带 [] 的 host 片段。
    MCQNET_NODISCARD
    static inline std::optional<SocketAddress> try_parse(std::string_view host, std::uint16_t port) noexcept {
        const std::string normalized = normalize_host_literal(host);
        if ( normalized.empty() ) {
            return std::nullopt;
        }

        sockaddr_in ipv4_address { };
        if ( inet_pton(AF_INET, normalized.c_str(), &ipv4_address.sin_addr) == 1 ) {
            ipv4_address.sin_family = AF_INET;
            ipv4_address.sin_port = htons(port);
            return from_ipv4_native(ipv4_address);
        }

        sockaddr_in6 ipv6_address { };
        if ( inet_pton(AF_INET6, normalized.c_str(), &ipv6_address.sin6_addr) == 1 ) {
            ipv6_address.sin6_family = AF_INET6;
            ipv6_address.sin6_port = htons(port);
            return from_ipv6_native(ipv6_address);
        }

        return std::nullopt;
    }

    MCQNET_NODISCARD
    static inline SocketAddress parse(std::string_view host, std::uint16_t port) {
        auto parsed = try_parse(host, port);
        if ( !parsed.has_value() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "SocketAddress::parse() requires an IP literal");
        }
        return *parsed;
    }

    MCQNET_NODISCARD
    static inline std::optional<SocketAddress> try_from_native(
        const sockaddr* address, native_size_type address_length) noexcept {
        if ( address == nullptr || address_length <= 0 ) {
            return std::nullopt;
        }

        if ( address->sa_family == AF_INET && address_length >= static_cast<native_size_type>(sizeof(sockaddr_in)) ) {
            sockaddr_in ipv4_address { };
            std::memcpy(&ipv4_address, address, sizeof(ipv4_address));
            return from_ipv4_native(ipv4_address);
        }

        if ( address->sa_family == AF_INET6 && address_length >= static_cast<native_size_type>(sizeof(sockaddr_in6)) ) {
            sockaddr_in6 ipv6_address { };
            std::memcpy(&ipv6_address, address, sizeof(ipv6_address));
            return from_ipv6_native(ipv6_address);
        }

        return std::nullopt;
    }

    MCQNET_NODISCARD
    static inline SocketAddress from_native(const sockaddr* address, native_size_type address_length) {
        auto parsed = try_from_native(address, address_length);
        if ( !parsed.has_value() ) {
            core::throw_net_error(
                core::error_code { core::errc::invalid_argument }, "SocketAddress::from_native() received an unsupported sockaddr");
        }
        return *parsed;
    }

    // valid() 的定义是 length_ != 0，而不是 family 合法。
    // 交接实现时若直接写 storage_，记得同步 length_，否则对象会一直被视为 invalid。
    MCQNET_NODISCARD
    inline bool valid() const noexcept { return length_ != 0; }

    MCQNET_NODISCARD
    inline explicit operator bool() const noexcept { return valid(); }

    MCQNET_NODISCARD
    inline AddressFamily family() const noexcept {
        if ( !valid() ) {
            return AddressFamily::unspecified;
        }
        if ( storage_.ss_family == AF_INET ) {
            return AddressFamily::ipv4;
        }
        if ( storage_.ss_family == AF_INET6 ) {
            return AddressFamily::ipv6;
        }
        return AddressFamily::unspecified;
    }

    MCQNET_NODISCARD
    inline bool is_ipv4() const noexcept { return family() == AddressFamily::ipv4; }

    MCQNET_NODISCARD
    inline bool is_ipv6() const noexcept { return family() == AddressFamily::ipv6; }

    MCQNET_NODISCARD
    inline std::uint16_t port() const noexcept {
        if ( is_ipv4() ) {
            return ntohs(as_ipv4()->sin_port);
        }
        if ( is_ipv6() ) {
            return ntohs(as_ipv6()->sin6_port);
        }
        return 0;
    }

    inline void set_port(std::uint16_t port) noexcept {
        if ( is_ipv4() ) {
            as_ipv4_mut()->sin_port = htons(port);
        } else if ( is_ipv6() ) {
            as_ipv6_mut()->sin6_port = htons(port);
        }
    }

    MCQNET_NODISCARD
    inline std::uint32_t scope_id() const noexcept { return is_ipv6() ? as_ipv6()->sin6_scope_id : 0; }

    MCQNET_NODISCARD
    inline native_size_type size() const noexcept { return length_; }

    MCQNET_NODISCARD
    static constexpr native_size_type capacity() noexcept { return static_cast<native_size_type>(sizeof(sockaddr_storage)); }

    // data()/size() 这一对就是传给 bind/connect/accept/getsockname 的标准入口。
    // 不要在上层自行 reinterpret_cast 内部存储，统一从这里取。
    MCQNET_NODISCARD
    inline const sockaddr* data() const noexcept {
        return valid() ? reinterpret_cast<const sockaddr*>(&storage_) : nullptr;
    }

    MCQNET_NODISCARD
    inline sockaddr* data() noexcept { return valid() ? reinterpret_cast<sockaddr*>(&storage_) : nullptr; }

    MCQNET_NODISCARD
    inline std::array<std::uint8_t, 4> ipv4_bytes() const noexcept {
        std::array<std::uint8_t, 4> bytes { };
        if ( is_ipv4() ) {
            std::memcpy(bytes.data(), &as_ipv4()->sin_addr, bytes.size());
        }
        return bytes;
    }

    MCQNET_NODISCARD
    inline std::array<std::uint8_t, 16> ipv6_bytes() const noexcept {
        std::array<std::uint8_t, 16> bytes { };
        if ( is_ipv6() ) {
            std::memcpy(bytes.data(), &as_ipv6()->sin6_addr, bytes.size());
        }
        return bytes;
    }

    MCQNET_NODISCARD
    inline std::string ip_string() const {
        if ( !valid() ) {
            return { };
        }

        char buffer[INET6_ADDRSTRLEN] { };
        if ( is_ipv4() ) {
            const char* text = inet_ntop(AF_INET, &as_ipv4()->sin_addr, buffer, sizeof(buffer));
            return text != nullptr ? std::string(text) : std::string();
        }

        if ( is_ipv6() ) {
            const char* text = inet_ntop(AF_INET6, &as_ipv6()->sin6_addr, buffer, sizeof(buffer));
            return text != nullptr ? std::string(text) : std::string();
        }

        return { };
    }

    MCQNET_NODISCARD
    inline std::string to_string() const {
        const std::string host = ip_string();
        if ( host.empty() ) {
            return { };
        }
        if ( is_ipv6() ) {
            return "[" + host + "]:" + std::to_string(port());
        }
        return host + ":" + std::to_string(port());
    }

private:
    MCQNET_NODISCARD
    static inline std::string normalize_host_literal(std::string_view host) {
        // 支持把常见的 "[::1]" host 片段归一化为 "::1" 后再走 inet_pton。
        if ( host.size() >= 2 && host.front() == '[' && host.back() == ']' ) {
            host.remove_prefix(1);
            host.remove_suffix(1);
        }
        return std::string(host);
    }

    MCQNET_NODISCARD
    static inline SocketAddress from_ipv4_native(const sockaddr_in& address) noexcept {
        SocketAddress socket_address;
        socket_address.length_ = static_cast<native_size_type>(sizeof(sockaddr_in));
        std::memcpy(&socket_address.storage_, &address, sizeof(address));
        return socket_address;
    }

    MCQNET_NODISCARD
    static inline SocketAddress from_ipv6_native(const sockaddr_in6& address) noexcept {
        SocketAddress socket_address;
        socket_address.length_ = static_cast<native_size_type>(sizeof(sockaddr_in6));
        std::memcpy(&socket_address.storage_, &address, sizeof(address));
        return socket_address;
    }

    MCQNET_NODISCARD
    inline const sockaddr_in* as_ipv4() const noexcept {
        return reinterpret_cast<const sockaddr_in*>(&storage_);
    }

    MCQNET_NODISCARD
    inline sockaddr_in* as_ipv4_mut() noexcept { return reinterpret_cast<sockaddr_in*>(&storage_); }

    MCQNET_NODISCARD
    inline const sockaddr_in6* as_ipv6() const noexcept {
        return reinterpret_cast<const sockaddr_in6*>(&storage_);
    }

    MCQNET_NODISCARD
    inline sockaddr_in6* as_ipv6_mut() noexcept { return reinterpret_cast<sockaddr_in6*>(&storage_); }

private:
    sockaddr_storage storage_ { };
    native_size_type length_ { 0 };
};

inline bool operator==(const SocketAddress& lhs, const SocketAddress& rhs) noexcept {
    if ( lhs.family() != rhs.family() || lhs.port() != rhs.port() ) {
        return false;
    }

    if ( lhs.is_ipv4() ) {
        return lhs.ipv4_bytes() == rhs.ipv4_bytes();
    }

    if ( lhs.is_ipv6() ) {
        return lhs.scope_id() == rhs.scope_id() && lhs.ipv6_bytes() == rhs.ipv6_bytes();
    }

    return !lhs.valid() && !rhs.valid();
}

inline bool operator!=(const SocketAddress& lhs, const SocketAddress& rhs) noexcept { return !(lhs == rhs); }

} // namespace mcqnet::net

namespace mcqnet {

using net::AddressFamily;
using net::SocketAddress;

} // namespace mcqnet
