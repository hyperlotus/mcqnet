#pragma once

// Handle 的 canonical 公共头。
// 这里保留 `mcqnet::runtime::Handle` 这个稳定入口，
// 用来表达“非 owning 的 runtime 引用视图”这层语义，
// 因此这里先提供稳定的 include 路径。
//
// 当前 Handle 与 Runtime 仍共用同一个实现头：
// - 最小 ready queue runtime 还是单头实现，先避免过早拆分
// - 保持 `mcqnet/runtime/handle.h` 这条公开路径已经可用
// - 后续若拆分定义，不需要改动调用方 include

#include <mcqnet/runtime/runtime.h>
