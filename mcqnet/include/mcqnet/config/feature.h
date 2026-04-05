#pragma once

// 语言/标准库特性探测

#include <mcqnet/config/platform.h>
#include <version>

#if defined(__cplusplus)
#define MCQNET_CXX_VERSION __cplusplus
#else
#define MCQNET_CXX_VERSION 0L
#endif

// coroutine 支持检测

#if defined(__cpp_impl_coroutine) || defined(__cpp_lib_coroutine)
#define MCQNET_HAS_COROUTINE 1
#else
#define MCQNET_HAS_COROUTINE 0
#endif

// exception 支持检测

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define MCQNET_HAS_EXCEPTIONS 1
#else
#define MCQNET_HAS_EXCEPTIONS 0
#endif

// source_location 支持检测

#if defined(__cpp_lib_source_location) && (__cpp_lib_source_location >= 201907L)
#define MCQNET_HAS_SOURCE_LOCATION 1
#else
#define MCQNET_HAS_SOURCE_LOCATION 0
#endif

// liburing 头可用性检测

#if MCQNET_PLATFORM_LINUX
#if defined(__has_include)
#if __has_include(<liburing.h>)
#define MCQNET_HAS_LIBURING 1
#else
#define MCQNET_HAS_LIBURING 0
#endif
#else
#define MCQNET_HAS_LIBURING 0
#endif
#else
#define MCQNET_HAS_LIBURING 0
#endif
namespace mcqnet {
// 公开的特性常量，便于用户代码按编译期能力做裁剪。
inline constexpr bool has_coroutine = (MCQNET_HAS_COROUTINE == 1);
inline constexpr bool has_exceptions = (MCQNET_HAS_EXCEPTIONS == 1);
inline constexpr bool has_source_location = (MCQNET_HAS_SOURCE_LOCATION == 1);
inline constexpr bool has_liburing = (MCQNET_HAS_LIBURING == 1);
} // namespace mcqnet
