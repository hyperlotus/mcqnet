#include <cassert>
#include <iostream>
#include <mcqnet/memory/thread_local_pool.h>
#include <thread>

int main() {
    using mcqnet::memory::ThreadLocalPool;

    void* leaked_from_dead_thread = nullptr;

    {
        std::thread t([&] {
            leaked_from_dead_thread = ThreadLocalPool::local().allocate(128);
            assert(leaked_from_dead_thread != nullptr);
            // 线程退出，ThreadLocalPool 析构
        });
        t.join();
    }

    // 原线程已经结束，此时再由其他线程释放
    {
        std::thread t([&] { ThreadLocalPool::local().deallocate(leaked_from_dead_thread); });
        t.join();
    }

    std::cout << "thread-exit-safe deallocation test passed\n";
    return 0;
}
