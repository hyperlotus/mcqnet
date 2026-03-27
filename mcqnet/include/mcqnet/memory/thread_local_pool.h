#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/memory/fixed_block_pool.h>

namespace mcqnet::memory {

// ThreadLocalPool 是面向调用方的通用小对象分配入口。
// 设计上分成两层：
// 1. 小对象路径：按 size class 路由到当前线程拥有的一组 FixedBlockPool
// 2. fallback 路径：对于大对象或超大对齐需求，走单独的原始分配
//
// 不论走哪条路径，返回给用户的指针前方都会放置 AllocationPrefix，
// 这样 deallocate()/usable_size() 都能从用户指针反查 AllocationHeader，
// 无需调用方额外传入大小或来源信息。
class ThreadLocalPool {
public:
    // 每个线程首次访问 local() 时都会构造一个独立实例，
    // 并一次性初始化所有 size class 对应的固定块池。
    ThreadLocalPool() { initialize_pools(); }

    ThreadLocalPool(const ThreadLocalPool&) = delete;
    ThreadLocalPool& operator=(const ThreadLocalPool&) = delete;

    ~ThreadLocalPool() = default;

    MCQNET_NODISCARD
    static inline ThreadLocalPool& local() {
        thread_local ThreadLocalPool pool;
        return pool;
    }

    // 通用分配入口。
    // - size == 0 时按 1 字节处理，保证返回可释放的非空存储
    // - 小对象且对齐要求不超过 max_align_t 时走固定块池
    // - 其余情况走 fallback_allocate()
    MCQNET_NODISCARD
    inline void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t)) {
        if ( size == 0 ) {
            size = 1;
        }

        if ( can_use_small_pool(size, align) ) {
            const std::size_t index = size_class_index(size);
            return pools_[index].allocate();
        }

        return fallback_allocate(size, align);
    }

    // 通用释放入口。
    // 释放时不需要 size/align 参数，因为它们都已经编码在 AllocationHeader 中：
    // - owner != nullptr: 说明来自某个 FixedBlockPool，交给其统一回收逻辑
    // - owner == nullptr: 说明是 fallback 分配，直接释放原始内存
    inline void deallocate(void* ptr) noexcept {
        if ( ptr == nullptr ) {
            return;
        }

        AllocationHeader* header = header_from_user_pointer(ptr);
        if ( header == nullptr ) {
            return;
        }

#if MCQNET_DEBUG
        MCQNET_ASSERT(header->magic == allocation_magic);
#endif

        if ( header->owner != nullptr ) {
            FixedBlockPool::deallocate_header(header);
            return;
        }

        fallback_deallocate(header);
    }

    // 返回分配时记录的可用大小：
    // - 小对象路径返回对应 size class 的块大小
    // - fallback 路径返回原始请求大小
    MCQNET_NODISCARD
    inline std::size_t usable_size(void* ptr) const noexcept {
        if ( ptr == nullptr ) {
            return 0;
        }

        AllocationHeader* header = header_from_user_pointer(ptr);
        return header != nullptr ? static_cast<std::size_t>(header->usable_size) : 0;
    }

    // allocate() + placement new 的对象构造便捷接口。
    // 若构造过程抛异常，已申请的原始内存会立刻释放。
    template <typename T, typename... Args> MCQNET_NODISCARD inline T* make(Args&&... args) {
        static_assert(!std::is_array_v<T>, "array types are not supported by make<T>()");

        void* memory = allocate(sizeof(T), alignof(T));
        try {
            return new (memory) T(std::forward<Args>(args)...);
        } catch ( ... ) {
            deallocate(memory);
            throw;
        }
    }

    // 与 make<T>() 配对的对象级释放接口：
    // 先执行析构，再将底层存储退回对应的池或 fallback 分配。
    template <typename T> inline void destroy(T* ptr) noexcept {
        if ( ptr == nullptr ) {
            return;
        }

        ptr->~T();
        deallocate(ptr);
    }

    // 小对象路径的上限。超过该尺寸或对齐要求过高时转入 fallback 分配。
    static inline constexpr std::size_t small_object_max_size = 4096;

private:
    // 预定义的 size class。
    // allocate() 会选择“首个大于等于请求大小”的档位，因此 usable_size()
    // 对小对象可能大于调用方申请的 size。
    inline static constexpr std::array<std::size_t, 15> size_classes_
        = { 16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096 };

    // 每个 ThreadLocalPool 实例独占一组固定块池，不与其他线程共享 local freelist。
    std::array<FixedBlockPool, size_classes_.size()> pools_ { };

private:
    MCQNET_NODISCARD
    static inline constexpr bool can_use_small_pool(std::size_t size, std::size_t align) noexcept {
        return align <= alignof(std::max_align_t) && size <= small_object_max_size;
    }

    // 为所有 size class 预建控制块；chunk 仍然按需懒分配。
    inline void initialize_pools() {
        for ( std::size_t i = 0; i < size_classes_.size(); ++i ) {
            pools_[i].initialize(size_classes_[i], blocks_per_chunk_for(size_classes_[i]));
        }
    }

    MCQNET_NODISCARD
    static inline constexpr std::size_t blocks_per_chunk_for(std::size_t size_class) noexcept {
        // 小块更密集，减少 chunk 元数据摊销；
        // 大块降低单次 chunk 分配体积，避免过度预留内存。
        if ( size_class <= 64 ) {
            return 256;
        }
        if ( size_class <= 256 ) {
            return 128;
        }
        if ( size_class <= 1024 ) {
            return 64;
        }
        return 32;
    }

    MCQNET_NODISCARD
    static inline constexpr std::size_t size_class_index(std::size_t size) noexcept {
        // 线性扫描足够简单，size class 个数固定且很小。
        for ( std::size_t i = 0; i < size_classes_.size(); ++i ) {
            if ( size <= size_classes_[i] ) {
                return i;
            }
        }
        return size_classes_.size() - 1;
    }

    MCQNET_NODISCARD
    static inline AllocationHeader* header_from_user_pointer(void* ptr) noexcept {
        // 用户指针前紧邻 AllocationPrefix，因此可以 O(1) 反查头部元数据。
        auto* user = reinterpret_cast<std::byte*>(ptr);
        auto* prefix = reinterpret_cast<AllocationPrefix*>(user - sizeof(AllocationPrefix));
        return prefix->header;
    }

    // fallback 路径：
    // - 大对象
    // - 超过 max_align_t 的特殊对齐对象
    //
    // 布局：
    // [AllocationHeader][padding...][AllocationPrefix][user_memory]
    //
    // 这里先申请一块 max_align_t 对齐的大缓冲区，再在缓冲区内部手动对齐 user_memory，
    // 因此即使请求对齐值大于 max_align_t，也能通过额外 padding 满足对齐约束。
    MCQNET_NODISCARD
    static inline void* fallback_allocate(std::size_t size, std::size_t align) {
        if ( align < alignof(void*) ) {
            align = alignof(void*);
        }

        const std::size_t total = sizeof(AllocationHeader) + sizeof(AllocationPrefix) + size + align;

        void* raw = nullptr;
        try {
            raw = ::operator new(total, std::align_val_t { alignof(std::max_align_t) });
        } catch ( const std::bad_alloc& ) {
            throw_memory_error(error_code { errc::out_of_memory, 0 }, "ThreadLocalPool fallback allocation failed");
        }

        auto* base = reinterpret_cast<std::byte*>(raw);
        auto* header = reinterpret_cast<AllocationHeader*>(base);
        auto* after_header = base + sizeof(AllocationHeader);

        const std::uintptr_t candidate = reinterpret_cast<std::uintptr_t>(after_header + sizeof(AllocationPrefix));
        const std::uintptr_t aligned_user = (candidate + align - 1u) & ~(static_cast<std::uintptr_t>(align) - 1u);

        auto* prefix = reinterpret_cast<AllocationPrefix*>(aligned_user - sizeof(AllocationPrefix));
        void* user_ptr = reinterpret_cast<void*>(aligned_user);

        header->owner = nullptr;
        header->chunk = nullptr;
        header->next = nullptr;
        header->raw_allocation = raw;
        header->usable_size = static_cast<std::uint32_t>(size);
        header->flags = 0;

#if MCQNET_DEBUG
        header->magic = allocation_magic;
        header->state.store(static_cast<std::uint32_t>(AllocationState::fallback), std::memory_order_relaxed);
#endif

        prefix->header = header;
        return user_ptr;
    }

    // fallback 分配的释放不经过固定块池，直接回收到原始 operator new 缓冲区。
    static inline void fallback_deallocate(AllocationHeader* header) noexcept {
#if MCQNET_DEBUG
        const auto old_state
            = header->state.exchange(static_cast<std::uint32_t>(AllocationState::free), std::memory_order_acq_rel);
        MCQNET_ASSERT(old_state == static_cast<std::uint32_t>(AllocationState::fallback));
#endif
        void* raw = header->raw_allocation;
        ::operator delete(raw, std::align_val_t { alignof(std::max_align_t) });
    }
};

} // namespace mcqnet::memory
