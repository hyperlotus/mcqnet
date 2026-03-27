#pragma once

// 平台探测层：

#if defined(_WIN32) || defined(_WIN64)
#define MCQNET_PLATFORM_WINDOWS 1
#else
#define MCQNET_PLATFORM_WINDOWS 0
#endif

#if defined(__linux__)
#define MCQNET_PLATFORM_LINUX 1
#else
#define MCQNET_PLATFORM_LINUX 0
#endif

#if !MCQNET_PLATFORM_LINUX && !MCQNET_PLATFORM_WINDOWS
#error "mcqnet currently supports only Windows and Linux."
#endif

#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__) || defined(__ppc64__)
#define MCQNET_ARCH_64BIT 1
#else
#define MCQNET_ARCH_64BIT 0
#endif
#if !MCQNET_ARCH_64BIT
#define MCQNET_ARCH_32BIT 1
#else
#define MCQNET_ARCH_32BIT 0
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define MCQNET_BIG_ENDIAN 1
#define MCQNET_LITTLE_ENDIAN 0
#else
#define MCQNET_BIG_ENDIAN 0
#define MCQNET_LITTLE_ENDIAN 1
#endif

namespace mcqnet {
// 平台与字节序常量，供上层以 constexpr 方式选择实现。
constexpr inline bool is_windows = MCQNET_PLATFORM_WINDOWS == 1;
constexpr inline bool is_linux = MCQNET_PLATFORM_LINUX == 1;
constexpr inline bool is_64bit = MCQNET_ARCH_64BIT == 1;
constexpr inline bool is_32bit = MCQNET_ARCH_32BIT == 1;
constexpr inline bool is_le = MCQNET_LITTLE_ENDIAN == 1;
constexpr inline bool is_be = MCQNET_BIG_ENDIAN == 1;
} // namespace mcqnet
