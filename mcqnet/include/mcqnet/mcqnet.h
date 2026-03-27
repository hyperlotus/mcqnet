#pragma once

// mcqnet 统一入口头文件。
// 当前聚合的是已经相对稳定的公共模块：
// - core
// - memory
// - task
// - runtime（目前是最小单线程 ready queue 版本）
//
// detail 仍然保持“按需直接包含”的策略，不在这里完整重导出。

#include <mcqnet/core/cacheline.h>
#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/macro.h>
#include <mcqnet/memory/fixed_block_pool.h>
#include <mcqnet/memory/object_pool.h>
#include <mcqnet/runtime/runtime.h>
#include <mcqnet/memory/thread_local_pool.h>
#include <mcqnet/task/spawn.h>
