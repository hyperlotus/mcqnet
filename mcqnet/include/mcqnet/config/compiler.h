#pragma once

// 编译器识别

#if defined(_MSC_VER)
#define MCQNET_COMPILER_MSVC 1
#else
#define MCQNET_COMPILER_MSVC 0
#endif
#if defined(__clang__)
#define MCQNET_COMPILER_CLANG 1
#else
#define MCQNET_COMPILER_CLANG 0
#endif
#if defined(__GNUC__) && !defined(__clang__)
#define MCQNET_COMPILER_GCC 1
#else
#define MCQNET_COMPILER_GCC 0
#endif

// 内联 / 不内联

#if MCQNET_COMPILER_MSVC
#define MCQNET_FORCEINLINE __forceinline
#define MCQNET_NOINLINE __declspec(noinline)
#elif MCQNET_COMPILER_CLANG || MCQNET_COMPILER_GCC
#define MCQNET_FORCEINLINE __attribute__((always_inline)) inline
#define MCQNET_NOINLINE __attribute__((noinline))
#else
#define MCQNET_FORCEINLINE inline
#define MCQNET_NOINLINE
#endif

// 分支预测

#if MCQNET_COMPILER_CLANG || MCQNET_COMPILER_GCC
#define MCQNET_LIKELY(x) (__builtin_expect(!!(x), 1))
#define MCQNET_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define MCQNET_LIKELY(x) (x)
#define MCQNET_UNLIKELY(x) (x)
#endif

// 提示调用方不要忽略返回值。
#define MCQNET_NODISCARD [[nodiscard]]

// 对齐辅助
// 目前统一使用 64 字节缓存行，后续如果需要可按平台做更细粒度区分。
#define MCQNET_CACHELINE_SIZE 64
#define MCQNET_CACHELINE_ALIGNED alignas(MCQNET_CACHELINE_SIZE)

namespace mcqnet {
// 便于以 constexpr 方式在模板或 if constexpr 中分派平台分支。
inline constexpr bool compiler_msvc = (MCQNET_COMPILER_MSVC == 1);
inline constexpr bool compiler_clang = (MCQNET_COMPILER_CLANG == 1);
inline constexpr bool compiler_gcc = (MCQNET_COMPILER_GCC == 1);
} // namespace mcqnet
