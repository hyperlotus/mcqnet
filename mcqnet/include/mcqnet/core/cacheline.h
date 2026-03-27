#pragma once

#include <cstddef>
#include <mcqnet/detail/macro.h>

namespace mcqnet::core {

constexpr inline std::size_t cacheline_size = MCQNET_CACHELINE_SIZE;

template <typename T> struct CachePadded {
    alignas(cacheline_size) T value;
};

} // namespace mcqnet::core

namespace mcqnet {

using core::CachePadded;
using core::cacheline_size;

} // namespace mcqnet

namespace mcqnet::memory {

using ::mcqnet::core::CachePadded;
using ::mcqnet::core::cacheline_size;

} // namespace mcqnet::memory
