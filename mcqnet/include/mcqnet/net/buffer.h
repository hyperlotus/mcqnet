#pragma once

// 网络 IO 的轻量 buffer view。
// 这里先提供和 read/write/recv/send 对齐的“指针 + 字节数”形状，
// 后续具体 socket awaitable 直接复用它们。

#include <cstddef>
#include <mcqnet/detail/macro.h>
#include <span>

namespace mcqnet::net {

struct MutableBuffer {
    void* data { nullptr };
    std::size_t size { 0 };

    MCQNET_NODISCARD
    constexpr bool empty() const noexcept { return size == 0; }

    MCQNET_NODISCARD
    constexpr explicit operator bool() const noexcept { return data != nullptr || size == 0; }
};

struct ConstBuffer {
    const void* data { nullptr };
    std::size_t size { 0 };

    MCQNET_NODISCARD
    constexpr bool empty() const noexcept { return size == 0; }

    MCQNET_NODISCARD
    constexpr explicit operator bool() const noexcept { return data != nullptr || size == 0; }
};

template <typename T>
MCQNET_NODISCARD inline MutableBuffer buffer(std::span<T> values) noexcept {
    return MutableBuffer { static_cast<void*>(values.data()), values.size_bytes() };
}

template <typename T>
MCQNET_NODISCARD inline ConstBuffer buffer(std::span<const T> values) noexcept {
    return ConstBuffer { static_cast<const void*>(values.data()), values.size_bytes() };
}

template <typename T, std::size_t N>
MCQNET_NODISCARD inline MutableBuffer buffer(T (&values)[N]) noexcept {
    return buffer(std::span<T> { values });
}

template <typename T, std::size_t N>
MCQNET_NODISCARD inline ConstBuffer buffer(const T (&values)[N]) noexcept {
    return buffer(std::span<const T> { values });
}

} // namespace mcqnet::net

namespace mcqnet {

using net::ConstBuffer;
using net::MutableBuffer;
using net::buffer;

} // namespace mcqnet
