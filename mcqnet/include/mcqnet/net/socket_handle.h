#pragma once

// 轻量 native socket handle 包装。
// 当前阶段它只负责表达“是否持有一个可用的 native handle”，
// 不承担 close/dup 等平台资源管理逻辑。

#include <mcqnet/detail/macro.h>

namespace mcqnet::net {

class SocketHandle {
public:
#if MCQNET_PLATFORM_WINDOWS
    using native_handle_type = std::uintptr_t;
#else
    using native_handle_type = int;
#endif

    constexpr SocketHandle() noexcept = default;

    constexpr explicit SocketHandle(native_handle_type native_handle) noexcept
        : native_handle_(native_handle) { }

    MCQNET_NODISCARD
    static constexpr native_handle_type invalid_native_handle() noexcept {
#if MCQNET_PLATFORM_WINDOWS
        return std::numeric_limits<native_handle_type>::max();
#else
        return static_cast<native_handle_type>(-1);
#endif
    }

    MCQNET_NODISCARD
    constexpr bool valid() const noexcept { return native_handle_ != invalid_native_handle(); }

    MCQNET_NODISCARD
    constexpr explicit operator bool() const noexcept { return valid(); }

    MCQNET_NODISCARD
    constexpr native_handle_type native_handle() const noexcept { return native_handle_; }

    constexpr void reset(native_handle_type native_handle = invalid_native_handle()) noexcept { native_handle_ = native_handle; }

    MCQNET_NODISCARD
    constexpr native_handle_type release() noexcept {
        const native_handle_type native_handle = native_handle_;
        native_handle_ = invalid_native_handle();
        return native_handle;
    }

    constexpr void swap(SocketHandle& other) noexcept {
        const native_handle_type native_handle = native_handle_;
        native_handle_ = other.native_handle_;
        other.native_handle_ = native_handle;
    }

private:
    native_handle_type native_handle_ { invalid_native_handle() };
};

inline constexpr bool operator==(SocketHandle lhs, SocketHandle rhs) noexcept {
    return lhs.native_handle() == rhs.native_handle();
}

inline constexpr bool operator!=(SocketHandle lhs, SocketHandle rhs) noexcept {
    return !(lhs == rhs);
}

inline constexpr void swap(SocketHandle& lhs, SocketHandle& rhs) noexcept { lhs.swap(rhs); }

} // namespace mcqnet::net

namespace mcqnet {

using net::SocketHandle;

} // namespace mcqnet
