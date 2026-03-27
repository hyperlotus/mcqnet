#include <cassert>
#include <iostream>
#include <thread>

#include <mcqnet/mcqnet.h>

struct foo {
    int x { 1 };
    int y { 2 };
};

int main() {
    using mcqnet::memory::ThreadLocalPool;

    auto& pool = ThreadLocalPool::local();

    void* p1 = pool.allocate(32);
    assert(p1 != nullptr);
    pool.deallocate(p1);

    foo* f = pool.make<foo>();
    assert(f->x == 1);
    pool.destroy(f);

    void* cross = nullptr;

    {
        std::jthread t1([&] { cross = ThreadLocalPool::local().allocate(128); });
    }

    {
        std::jthread t2([&] { ThreadLocalPool::local().deallocate(cross); });
    }

    std::cout << "memory pool test passed\n";
    return 0;
}