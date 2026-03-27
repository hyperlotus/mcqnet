#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>

#include <mcqnet/core/error.h>
#include <mcqnet/core/exception.h>
#include <mcqnet/detail/macro.h>

namespace mcqnet::memory {

// FixedBlockPool 是单一块大小的分配器。
// 设计目标：
// - 当前线程上的 allocate() 和同线程 deallocate() 尽量走无锁快路径
// - 允许其他线程归还块，但跨线程释放只做无锁挂链，不直接触碰 owner 的 local freelist
// - 即使 thread_local 包装对象先析构，仍允许悬挂在外部的块在之后安全归还
//
// 为了支持最后一点，真正的共享状态都放在 PoolControl/ChunkControl 中，
// FixedBlockPool 这个外层对象只是一个拥有 control_ 指针的轻量句柄。
struct PoolControl;
struct ChunkControl;
struct AllocationPrefix;

// debug 状态机：
// - free: 已经回到池中，尚未发放给用户
// - allocated: 已发放给用户，等待归还
// - fallback: 被 ThreadLocalPool 的 fallback 路径借用
enum class AllocationState : std::uint32_t { free = 0, allocated = 1, fallback = 2 };

inline constexpr std::uint32_t allocation_magic = 0x4D43514Eu; // "MCQN"

// 每个分配块的头。
// 说明：
// 1. owner 指向独立控制块，不依赖 thread_local 对象本体生命周期
// 2. chunk 用于做 live_count 管理
// 3. next 复用为 freelist / remote_free_list 单链表指针
// 4. magic/state 用于调试校验
struct AllocationHeader {
    PoolControl* owner { nullptr };
    ChunkControl* chunk { nullptr };
    AllocationHeader* next { nullptr };
    void* raw_allocation { nullptr };
    std::uint32_t usable_size { 0 };
    std::uint32_t flags { 0 };

#if MCQNET_DEBUG
    std::uint32_t magic { allocation_magic };
    std::atomic<std::uint32_t> state { static_cast<std::uint32_t>(AllocationState::free) };
#endif
};

struct AllocationPrefix {
    AllocationHeader* header { nullptr };
};

// 一个 chunk 包含多个 block。
// 其中：
// - raw_memory 指向整块 chunk 的起始地址，便于最终整体释放
// - live_count 表示当前仍被调用方持有、尚未归还的块数
struct ChunkControl {
    ChunkControl* next { nullptr };
    PoolControl* owner { nullptr };
    void* raw_memory { nullptr };
    std::size_t total_bytes { 0 };
    std::uint32_t block_count { 0 };
    std::atomic<std::uint32_t> live_count { 0 };
};

// 固定块池的独立控制块。
// 正常运行阶段：
// - local_free_list 只由 owner 线程访问
// - remote_free_list 由其他线程无锁 push，owner 线程批量 drain
//
// 退休阶段：
// - retired == true 后，不再允许新块进入 freelist
// - retired_chunk_mutex 用于保护 retired 后的 chunk 链回收
struct PoolControl {
    // 控制块引用计数：
    // - PoolControl 初始化时持有 1 个“池自身引用”
    // - 每次 allocate() 发出一个块，就额外 retain 一次
    // - 对应块归还后 release 一次
    // 这样即使 FixedBlockPool 本体销毁，control 仍会活到最后一个外部块归还。
    std::atomic<std::uint32_t> ref_count { 1 };
    std::atomic<bool> retired { false };

    // 远程归还链表：其他线程只 push，不直接修改 local_free_list。
    std::atomic<AllocationHeader*> remote_free_list { nullptr };

    std::thread::id owner_thread_id { };

    // block_user_size 是对外可用大小；
    // block_total_size 则包含块头/前缀/对齐补齐后的实际步长。
    std::size_t block_user_size { 0 };
    std::size_t block_total_size { 0 };
    std::size_t blocks_per_chunk { 0 };

    // chunks 只在 owner 线程扩展，在 retired 阶段配合 mutex 回收。
    ChunkControl* chunks { nullptr };
    AllocationHeader* local_free_list { nullptr };

    std::mutex retired_chunk_mutex;
};

// 每个实例只服务一个固定 user_block_size。
// 它不关心对象构造/析构，仅负责发放和回收原始内存块。
class FixedBlockPool {
public:
    FixedBlockPool() = default;

    FixedBlockPool(const FixedBlockPool&) = delete;
    FixedBlockPool& operator=(const FixedBlockPool&) = delete;

    ~FixedBlockPool() noexcept { shutdown(); }

    // 建立控制块并记录块大小/每个 chunk 的块数。
    // chunk 本身不会在 initialize() 时立刻分配，而是在第一次 allocate() 时按需创建。
    inline void initialize(std::size_t user_block_size, std::size_t blocks_per_chunk) {
        if ( control_ != nullptr ) {
            return;
        }

        auto* control = new PoolControl { };
        control->owner_thread_id = std::this_thread::get_id();
        control->block_user_size = align_up(user_block_size, alignof(std::max_align_t));
        control->block_total_size = align_up(
            sizeof(AllocationHeader) + sizeof(AllocationPrefix) + control->block_user_size, alignof(std::max_align_t));
        control->blocks_per_chunk = blocks_per_chunk;

        control_ = control;
    }

    MCQNET_NODISCARD
    inline std::size_t user_block_size() const noexcept { return control_ != nullptr ? control_->block_user_size : 0; }

    MCQNET_NODISCARD
    inline void* allocate() {
        PoolControl* control = control_;
        if ( MCQNET_UNLIKELY(control == nullptr) ) {
            throw_memory_error(error_code { errc::invalid_state, 0 }, "FixedBlockPool is not initialized");
        }

        if ( MCQNET_UNLIKELY(control->retired.load(std::memory_order_acquire)) ) {
            throw_memory_error(error_code { errc::invalid_state, 0 }, "allocate on retired FixedBlockPool");
        }

        // 正常快路径优先消耗 owner 线程本地 freelist。
        // 只有在本地没有可用块时才会：
        // 1. 先吸收 remote freelist
        // 2. 仍为空时再扩容新 chunk
        if ( control->local_free_list == nullptr ) {
            drain_remote_frees(control);
            if ( control->local_free_list == nullptr ) {
                allocate_chunk(control);
            }
        }

        AllocationHeader* header = control->local_free_list;
        control->local_free_list = header->next;
        header->next = nullptr;

        header->chunk->live_count.fetch_add(1, std::memory_order_relaxed);
        retain_pool(control);

#if MCQNET_DEBUG
        validate_header(header);
        const auto old_state
            = header->state.exchange(static_cast<std::uint32_t>(AllocationState::allocated), std::memory_order_acq_rel);
        MCQNET_ASSERT(old_state == static_cast<std::uint32_t>(AllocationState::free));
#endif

        // 返回给调用方的并不是 header 后的位置，而是 prefix 之后的用户区。
        // deallocate() 时会借助 prefix 再反查回 header。
        auto* prefix = prefix_from_header(header);
        prefix->header = header;
        return user_pointer_from_prefix(prefix);
    }

    // 统一释放入口：
    // - 正常阶段：同线程回 local freelist，跨线程进 remote freelist
    // - retired 阶段：不再进入 freelist，只做 live_count/chunk 回收
    static inline void deallocate_header(AllocationHeader* header) noexcept {
        if ( header == nullptr ) {
            return;
        }

#if MCQNET_DEBUG
        validate_header(header);
        const auto expected = static_cast<std::uint32_t>(AllocationState::allocated);
        const auto old_state
            = header->state.exchange(static_cast<std::uint32_t>(AllocationState::free), std::memory_order_acq_rel);
        MCQNET_ASSERT(old_state == expected);
#endif

        PoolControl* control = header->owner;
        ChunkControl* chunk = header->chunk;

        if ( control == nullptr || chunk == nullptr ) {
            return;
        }

        // 先读 retired。
        // 一旦看到 retired == true，后续绝不进入 freelist。
        const bool retired = control->retired.load(std::memory_order_acquire);

        if ( !retired ) {
            const bool same_thread = (std::this_thread::get_id() == control->owner_thread_id);

            if ( same_thread ) {
                header->next = control->local_free_list;
                control->local_free_list = header;
            } else {
                // push_remote 内部会再次检查 retired，避免与 shutdown 竞态
                if ( !push_remote_if_active(control, header) ) {
                    header->next = nullptr;
                }
            }
        } else {
            header->next = nullptr;
        }

        const std::uint32_t remain = chunk->live_count.fetch_sub(1, std::memory_order_acq_rel) - 1u;

        if ( remain == 0 ) {
            try_reclaim_chunk_if_retired(control, chunk);
        }

        release_pool(control);
    }

private:
    inline static constexpr std::size_t default_chunk_alignment = alignof(std::max_align_t);

    // 外层对象析构后会先把 control_ 置空，避免后续误用。
    PoolControl* control_ { nullptr };

private:
    MCQNET_NODISCARD
    static inline constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
        return (value + alignment - 1u) & ~(alignment - 1u);
    }

    MCQNET_NODISCARD
    static inline AllocationPrefix* prefix_from_header(AllocationHeader* header) noexcept {
        auto* base = reinterpret_cast<std::byte*>(header);
        return reinterpret_cast<AllocationPrefix*>(base + sizeof(AllocationHeader));
    }

    MCQNET_NODISCARD
    static inline void* user_pointer_from_prefix(AllocationPrefix* prefix) noexcept {
        auto* base = reinterpret_cast<std::byte*>(prefix);
        return static_cast<void*>(base + sizeof(AllocationPrefix));
    }

    // retain/release 维护的是 PoolControl 生命周期，而不是单个 chunk 生命周期。
    static inline void retain_pool(PoolControl* control) noexcept {
        control->ref_count.fetch_add(1, std::memory_order_relaxed);
    }

    static inline void release_pool(PoolControl* control) noexcept {
        const std::uint32_t remain = control->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1u;
        if ( remain == 0 ) {
            delete control;
        }
    }

#if MCQNET_DEBUG
    static inline void validate_header(const AllocationHeader* header) noexcept {
        MCQNET_ASSERT(header != nullptr);
        MCQNET_ASSERT(header->magic == allocation_magic);
    }
#endif

    // 只有在 pool 未退休时才能进入 remote freelist。
    // 为避免与 shutdown 的竞态，这里使用“双重检查”：
    // 1. 先看 retired
    // 2. CAS 入链
    // 3. 如果期间发现 retired 被置位，则尝试回滚并返回 false
    //
    // 注意：
    // 真正完全 lock-free 且严格线性化的回滚会非常复杂。
    // 这里采取工程上更稳妥的策略：
    // - retired 置位后，owner 不再 drain remote freelist
    // - shutdown 会原子摘取 remote freelist 并作为空闲块处理
    // - 远程线程 push 时再次检查 retired，尽量减少进入退休 freelist 的窗口
    static inline bool push_remote_if_active(PoolControl* control, AllocationHeader* header) noexcept {
        if ( control->retired.load(std::memory_order_acquire) ) {
            return false;
        }

        AllocationHeader* old_head = control->remote_free_list.load(std::memory_order_relaxed);

        do {
            header->next = old_head;
        } while ( !control->remote_free_list.compare_exchange_weak(
            old_head, header, std::memory_order_release, std::memory_order_relaxed) );

        // 入链后再看一次 retired。
        // 如果已经退休，该块仍然可能在 remote list 上，但 shutdown 会统一摘走并
        // 通过 live_count==0 的 chunk 回收机制释放相应 chunk。
        return !control->retired.load(std::memory_order_acquire);
    }

    // owner 线程批量吸收远程归还块。
    // 只有 active 状态才会被调用。
    static inline void drain_remote_frees(PoolControl* control) noexcept {
        AllocationHeader* list = control->remote_free_list.exchange(nullptr, std::memory_order_acquire);

        while ( list != nullptr ) {
            AllocationHeader* next = list->next;
            list->next = control->local_free_list;
            control->local_free_list = list;
            list = next;
        }
    }

    // 申请一个新 chunk，并把其中所有 block 串到 local_free_list。
    // 单个 block 的布局是：
    // [AllocationHeader][AllocationPrefix][aligned user storage]
    inline void allocate_chunk(PoolControl* control) {
        const std::size_t total_bytes = sizeof(ChunkControl) + control->block_total_size * control->blocks_per_chunk;

        void* raw = nullptr;
        try {
            raw = ::operator new(total_bytes, std::align_val_t { default_chunk_alignment });
        } catch ( const std::bad_alloc& ) {
            throw_memory_error(error_code { errc::out_of_memory, 0 }, "FixedBlockPool failed to allocate chunk");
        }

        auto* chunk = new (raw) ChunkControl { };
        chunk->owner = control;
        chunk->raw_memory = raw;
        chunk->total_bytes = total_bytes;
        chunk->block_count = static_cast<std::uint32_t>(control->blocks_per_chunk);
        chunk->live_count.store(0, std::memory_order_relaxed);

        chunk->next = control->chunks;
        control->chunks = chunk;

        // chunk 控制块之后紧跟一段连续 block 区域，每个 block 以固定步长切分。
        auto* block_base = reinterpret_cast<std::byte*>(chunk) + sizeof(ChunkControl);

        for ( std::size_t i = 0; i < control->blocks_per_chunk; ++i ) {
            auto* block = block_base + i * control->block_total_size;
            auto* header = new (block) AllocationHeader { };

            header->owner = control;
            header->chunk = chunk;
            header->next = control->local_free_list;
            header->raw_allocation = nullptr;
            header->usable_size = static_cast<std::uint32_t>(control->block_user_size);
            header->flags = 0;

#if MCQNET_DEBUG
            header->magic = allocation_magic;
            header->state.store(static_cast<std::uint32_t>(AllocationState::free), std::memory_order_relaxed);
#endif

            control->local_free_list = header;
        }
    }

    // 只有 retired 后才会尝试回收 chunk。
    // 进入退休态后，chunk 的回收条件变成：
    // 1. pool 已经 retired
    // 2. 该 chunk 的 live_count 归零
    //
    // 为避免多个线程并发摘除同一 chunk，这里用 mutex 保护 retired 阶段的 chunk 链。
    static inline void try_reclaim_chunk_if_retired(PoolControl* control, ChunkControl* target) noexcept {
        if ( !control->retired.load(std::memory_order_acquire) ) {
            return;
        }

        if ( target->live_count.load(std::memory_order_acquire) != 0 ) {
            return;
        }

        std::lock_guard<std::mutex> lock(control->retired_chunk_mutex);

        if ( target->live_count.load(std::memory_order_acquire) != 0 ) {
            return;
        }

        ChunkControl** current = &control->chunks;
        while ( *current != nullptr ) {
            if ( *current == target ) {
                *current = target->next;
                ::operator delete(target->raw_memory, std::align_val_t { default_chunk_alignment });
                return;
            }
            current = &((*current)->next);
        }
    }

    // shutdown 顺序非常关键：
    // 1. retired = true，阻止后续正常 freelist 路径
    // 2. 原子摘取 remote freelist，避免无人消费
    // 3. local_free_list 清空
    // 4. 释放 live_count == 0 的 chunk
    // 5. 其余 chunk 等最后一个外部块归还后回收
    //
    // shutdown 不会强行等待所有外部块归还，而是把池切换到 retired 模式，
    // 让后续 deallocate_header() 在最后一个引用归还时完成收尾回收。
    inline void shutdown() noexcept {
        PoolControl* control = control_;
        if ( control == nullptr ) {
            return;
        }

        control_ = nullptr;

        control->retired.store(true, std::memory_order_release);

        AllocationHeader* remote_list = control->remote_free_list.exchange(nullptr, std::memory_order_acquire);

        // 退休后 local freelist 不再保留。
        control->local_free_list = nullptr;

        // remote_list 里的块本质上已经是“空闲块”，它们不占 live_count。
        // 因此不需要逐块处理，只需要按 chunk 的 live_count==0 统一释放 chunk 即可。
        // 这里将链表断开，避免误用。
        while ( remote_list != nullptr ) {
            AllocationHeader* next = remote_list->next;
            remote_list->next = nullptr;
            remote_list = next;
        }

        {
            std::lock_guard<std::mutex> lock(control->retired_chunk_mutex);

            ChunkControl** current = &control->chunks;
            while ( *current != nullptr ) {
                ChunkControl* chunk = *current;
                if ( chunk->live_count.load(std::memory_order_acquire) == 0 ) {
                    *current = chunk->next;
                    ::operator delete(chunk->raw_memory, std::align_val_t { default_chunk_alignment });
                } else {
                    current = &((*current)->next);
                }
            }
        }

        release_pool(control);
    }
};

} // namespace mcqnet::memory
