#pragma once

#include <cstdint>
#include <mcqnet/config/compiler.h>
#include <mcqnet/detail/macro.h>
#include <string_view>

namespace mcqnet::core {

// 库内统一错误类别。
// value 表示跨平台语义，native 字段可补充底层系统错误码。
enum class errc : std::uint32_t {
    success = 0,
    // 通用
    unknown = 1,
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
    // 运行时
    runtime_not_initialized,
    runtime_stopped,
    scheduler_overloaded,
    task_cancelled,
    task_join_failed,
    // 内存
    out_of_memory,
    pool_exhausted,
    buffer_overflow,
    // 网络
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
    // IO
    io_error,
    short_read,
    short_write,
    // 平台后端
    iocp_error,
    uring_error,
    driver_error
};

// 错误码转字符串。
// 先用 switch 返回静态字符串，零分配、低开销。
MCQNET_NODISCARD
constexpr inline std::string_view to_string(errc ec) noexcept {
    switch ( ec ) {
        case errc::success: return "success";
        case errc::unknown: return "unknown";
        case errc::invalid_argument: return "invalid_argument";
        case errc::invalid_state: return "invalid_state";
        case errc::not_supported: return "not_supported";
        case errc::already_exists: return "already_exists";
        case errc::not_found: return "not_found";
        case errc::permission_denied: return "permission_denied";
        case errc::operation_aborted: return "operation_aborted";
        case errc::timed_out: return "timed_out";
        case errc::cancelled: return "cancelled";
        case errc::end_of_file: return "end_of_file";
        case errc::runtime_not_initialized: return "runtime_not_initialized";
        case errc::runtime_stopped: return "runtime_stopped";
        case errc::scheduler_overloaded: return "scheduler_overloaded";
        case errc::task_cancelled: return "task_cancelled";
        case errc::task_join_failed: return "task_join_failed";
        case errc::out_of_memory: return "out_of_memory";
        case errc::pool_exhausted: return "pool_exhausted";
        case errc::buffer_overflow: return "buffer_overflow";
        case errc::address_in_use: return "address_in_use";
        case errc::address_not_available: return "address_not_available";
        case errc::connection_refused: return "connection_refused";
        case errc::connection_reset: return "connection_reset";
        case errc::connection_aborted: return "connection_aborted";
        case errc::not_connected: return "not_connected";
        case errc::already_connected: return "already_connected";
        case errc::network_unreachable: return "network_unreachable";
        case errc::host_unreachable: return "host_unreachable";
        case errc::broken_pipe: return "broken_pipe";
        case errc::message_too_large: return "message_too_large";
        case errc::would_block: return "would_block";
        case errc::io_error: return "io_error";
        case errc::short_read: return "short_read";
        case errc::short_write: return "short_write";
        case errc::iocp_error: return "iocp_error";
        case errc::uring_error: return "uring_error";
        case errc::driver_error: return "driver_error";
        default: return "unrecognized_errc";
    }
}

// 轻量错误对象，设计上可按值传递。
struct error_code {
    errc value { errc::success };
    std::uint32_t native { 0 };
    constexpr error_code() noexcept = default;

    constexpr error_code(errc v, std::uint32_t n = 0) noexcept
        : value(v)
        , native(n) { }

    // 与 std::error_code 的习惯保持一致：true 表示存在错误。
    MCQNET_NODISCARD
    constexpr explicit operator bool() const noexcept { return value != errc::success; }

    MCQNET_NODISCARD
    constexpr bool success() const noexcept { return value == errc::success; }

    // 返回库内统一错误字符串，不做动态分配。
    MCQNET_NODISCARD
    constexpr std::string_view message() const noexcept { return to_string(value); }
};
} // namespace mcqnet::core

namespace mcqnet {

using core::errc;
using core::error_code;
using core::to_string;

} // namespace mcqnet
