#pragma once

// 统一断言：
// 1. Debug 模式下尽快暴露逻辑错误
// 2. Release 模式下尽量不引入额外开销

#include <cassert>

#if defined(NDEBUG)
#define MCQNET_DEBUG 0
#else
#define MCQNET_DEBUG 1
#endif

#if MCQNET_DEBUG
// Debug 构建下保留断言，尽量早暴露调用方和库内部的不变量问题。
#define MCQNET_ASSERT(expr) assert((expr))
#else
#define MCQNET_ASSERT(expr) ((void)0)
#endif

#if defined(_MSC_VER)
// 用于标记“逻辑上不可能到达”的分支，帮助编译器优化。
#define MCQNET_UNREACHABLE() __assume(0)
#elif defined(__clang__) || defined(__GNUC__)
#define MCQNET_UNREACHABLE() __builtin_unreachable()
#else
#define MCQNET_UNREACHABLE() ((void)0)
#endif
