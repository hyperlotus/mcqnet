#pragma once

#include <mcqnet/config/compiler.h>
#include <mcqnet/core/error.h>
#include <stdexcept>
#include <string>

namespace mcqnet::core {
// 所有 mcqnet 异常的基类，携带 error_code 便于程序化处理。
class Exception : public std::runtime_error {
public:
    explicit Exception(error_code ec, std::string what)
        : std::runtime_error(what)
        , code_(ec) { }

    MCQNET_NODISCARD
    const error_code& code() const noexcept { return code_; }

private:
    error_code code_;
};

class RuntimeException : public Exception {
public:
    explicit RuntimeException(error_code ec, std::string what_arg)
        : Exception(ec, std::move(what_arg)) { }
};

// 网络相关异常。
class NetException : public Exception {
public:
    explicit NetException(error_code ec, std::string what_arg)
        : Exception(ec, std::move(what_arg)) { }
};

// IO 相关异常。
class IOException : public Exception {
public:
    explicit IOException(error_code ec, std::string what_arg)
        : Exception(ec, std::move(what_arg)) { }
};

// 内存分配和内存池相关异常。
class MemoryException : public Exception {
public:
    explicit MemoryException(error_code ec, std::string what_arg)
        : Exception(ec, std::move(what_arg)) { }
};

[[noreturn]]
// 抛出运行时异常；若禁用异常支持，则走不可达路径，由上层自行保证不调用。
MCQNET_FORCEINLINE void throw_runtime_error(error_code ec, std::string msg) {
#if MCQNET_HAS_EXCEPTIONS
    throw RuntimeException(ec, std::move(msg));
#else
    (void)ec;
    (void)msg;
    MCQNET_UNREACHABLE();
#endif
}

[[noreturn]] MCQNET_FORCEINLINE void thorw_runtime_error(error_code ec, std::string msg) {
    throw_runtime_error(ec, std::move(msg));
}

// 抛出网络异常。
[[noreturn]] MCQNET_FORCEINLINE void throw_net_error(error_code ec, std::string msg) {
#if MCQNET_HAS_EXCEPTIONS
    throw NetException(ec, std::move(msg));
#else
    (void)ec;
    (void)msg;
    MCQNET_UNREACHABLE();
#endif
}

// 抛出 IO 异常。
[[noreturn]] MCQNET_FORCEINLINE void throw_io_error(error_code ec, std::string msg) {
#if MCQNET_HAS_EXCEPTIONS
    throw IOException(ec, std::move(msg));
#else
    (void)ec;
    (void)msg;
    MCQNET_UNREACHABLE();
#endif
}

// 抛出内存异常。
[[noreturn]] MCQNET_FORCEINLINE void throw_memory_error(error_code ec, std::string msg) {
#if MCQNET_HAS_EXCEPTIONS
    throw MemoryException(ec, std::move(msg));
#else
    (void)ec;
    (void)msg;
    MCQNET_UNREACHABLE();
#endif
}
} // namespace mcqnet::core

namespace mcqnet {

using core::Exception;
using core::IOException;
using core::MemoryException;
using core::NetException;
using core::RuntimeException;
using core::thorw_runtime_error;
using core::throw_io_error;
using core::throw_memory_error;
using core::throw_net_error;
using core::throw_runtime_error;

} // namespace mcqnet
