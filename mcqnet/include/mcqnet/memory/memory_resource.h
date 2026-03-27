#pragma once

// mcqnet 内存分配器适配 std::pmr（Polymorphic Memory Resource）接口。
// 可与 C++17 标准库容器（如 std::pmr::vector、std::pmr::string）配合使用。

#include <cstddef>
#include <mcqnet/memory/thread_local_pool.h>
#include <memory_resource>

namespace mcqnet::memory {

// MemoryResource 将 ThreadLocalPool 适配为 std::pmr::memory_resource。
// 分配：调用 ThreadLocalPool::local().allocate()
// 释放：调用 ThreadLocalPool::local().deallocate()
// 注意：不同线程的 local() 指向不同实例，线程亲源性与 ThreadLocalPool 一致。
class MemoryResource : public std::pmr::memory_resource {
public:
    MemoryResource() = default;
    ~MemoryResource() override = default;
    MemoryResource(const MemoryResource&) = delete;
    MemoryResource& operator=(const MemoryResource&) = delete;

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return ThreadLocalPool::local().allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t, std::size_t) override { ThreadLocalPool::local().deallocate(p); }

    bool do_is_equal(const std::pmr::memory_resource& rhs) const noexcept override {
        return this == std::addressof(rhs);
    }
};

using McqnetMemoryResource = MemoryResource;
} // namespace mcqnet::memory
