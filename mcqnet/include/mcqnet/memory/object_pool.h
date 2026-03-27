#pragma once

#include <mcqnet/detail/macro.h>
#include <mcqnet/memory/thread_local_pool.h>
#include <memory>
#include <type_traits>
#include <utility>

namespace mcqnet::memory {
// ObjectPool<T> 是 ThreadLocalPool 的强类型薄封装。
// 它本身不维护独立池实例，只负责把：
// 1. 申请原始内存
// 2. placement new 构造对象
// 3. 显式析构对象
// 4. 将底层内存交回 ThreadLocalPool
// 这一组操作整理成更容易正确使用的对象级 API。
//
// 因为底层仍然走 AllocationHeader 路由，所以对象可以在非创建线程销毁；
// 跨线程归还时会由 ThreadLocalPool / FixedBlockPool 自动转发回拥有者线程。
template <typename T> class ObjectPool {
public:
    static_assert(!std::is_array_v<T>, "object_pool<T[]> is not supported");
    ObjectPool() = default;
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // 申请一块足以容纳 T 的内存并原地构造对象。
    // 这里只做“分配 + 构造”，调用方需要显式配对 destroy()，
    // 或者使用 make_unique() 取得 RAII 包装。
    //
    // 异常安全：
    // 如果 T 的构造函数抛出异常，已经申请的原始内存会立即退回池中，
    // 不会泄漏半初始化对象对应的存储空间。
    template <typename... Args> MCQNET_NODISCARD inline T* create(Args&&... args) {
        void* memory = ThreadLocalPool::local().allocate(sizeof(T), alignof(T));
        try {
            return new (memory) T(std::forward<Args>(args)...);
        } catch ( ... ) {
            ThreadLocalPool::local().deallocate(memory);
            throw;
        }
    }

    // 显式析构对象，并把底层存储交回 ThreadLocalPool。
    // 如果 destroy() 发生在其他线程，底层池会根据块头信息决定：
    // - 同线程归还到本地 freelist
    // - 跨线程挂到 remote freelist，等待拥有者线程回收
    inline void destroy(T* ptr) noexcept {
        if ( ptr == nullptr ) {
            return;
        }
        ptr->~T();
        ThreadLocalPool::local().deallocate(ptr);
    }

    // 供 std::unique_ptr 使用的删除器，语义与 destroy() 完全一致。
    // 删除器本身无状态，因此 unique_ptr 只额外携带指针，不保存池句柄。
    struct deleter {
        inline void operator()(T* ptr) const noexcept {
            if ( ptr == nullptr ) {
                return;
            }
            ptr->~T();
            ThreadLocalPool::local().deallocate(ptr);
        }
    };

    using unique_ptr = std::unique_ptr<T, deleter>;

    // create() 的 RAII 版本。
    // 适合把“对象析构 + 内存归还”绑定到作用域结束，避免手工调用 destroy()。
    template <typename... Args> MCQNET_NODISCARD inline unique_ptr make_unique(Args&&... args) {
        return unique_ptr { create(std::forward<Args>(args)...) };
    }

    // 保留历史拼写接口，避免破坏已有调用点。
    template <typename... Args> MCQNET_NODISCARD inline unique_ptr make_unqiue(Args&&... args) {
        return make_unique(std::forward<Args>(args)...);
    }
};
} // namespace mcqnet::memory
