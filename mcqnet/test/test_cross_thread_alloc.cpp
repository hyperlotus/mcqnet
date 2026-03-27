#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include <mcqnet/mcqnet.h>

namespace {
struct SmallObject {
    std::uint64_t value { 0 };
    std::array<std::uint8_t, 56> payload { };
};

static_assert(sizeof(SmallObject) == 64);

using AddressList = std::vector<std::uintptr_t>;

constexpr std::size_t cross_thread_object_count = 256;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << message << '\n';
    std::abort();
}

void check(bool condition, std::string_view message) {
    if ( !condition ) {
        fail(message);
    }
}

AddressList sorted_addresses(const std::vector<SmallObject*>& objects) {
    AddressList addresses;
    addresses.reserve(objects.size());
    for ( SmallObject* ptr : objects ) {
        addresses.push_back(reinterpret_cast<std::uintptr_t>(ptr));
    }
    std::sort(addresses.begin(), addresses.end());
    return addresses;
}
} // namespace

int main() {
    using mcqnet::memory::ThreadLocalPool;

    std::promise<std::vector<SmallObject*>> allocated_promise;
    std::future<std::vector<SmallObject*>> allocated_future = allocated_promise.get_future();

    std::promise<void> released_promise;
    std::shared_future<void> released_future = released_promise.get_future().share();

    std::promise<AddressList> original_addresses_promise;
    std::future<AddressList> original_addresses_future = original_addresses_promise.get_future();

    std::promise<AddressList> recycled_addresses_promise;
    std::future<AddressList> recycled_addresses_future = recycled_addresses_promise.get_future();

    std::thread producer([allocated_promise = std::move(allocated_promise), released_future,
                             original_addresses_promise = std::move(original_addresses_promise),
                             recycled_addresses_promise = std::move(recycled_addresses_promise)]() mutable {
        auto& pool = ThreadLocalPool::local();

        // 64-byte objects currently use 256 blocks per chunk. Allocating exactly one chunk's worth ensures the
        // next owner-thread allocation pass must first drain the remote free list.
        std::vector<SmallObject*> allocated;
        allocated.reserve(cross_thread_object_count);
        for ( std::size_t i = 0; i < cross_thread_object_count; ++i ) {
            SmallObject* ptr = pool.make<SmallObject>();
            ptr->value = static_cast<std::uint64_t>(i);
            ptr->payload[i % ptr->payload.size()] = static_cast<std::uint8_t>(i);
            allocated.push_back(ptr);
        }

        original_addresses_promise.set_value(sorted_addresses(allocated));
        allocated_promise.set_value(std::move(allocated));

        released_future.wait();

        std::vector<SmallObject*> recycled;
        recycled.reserve(cross_thread_object_count);
        for ( std::size_t i = 0; i < cross_thread_object_count; ++i ) {
            recycled.push_back(pool.make<SmallObject>());
        }

        recycled_addresses_promise.set_value(sorted_addresses(recycled));

        for ( SmallObject* ptr : recycled ) {
            pool.destroy(ptr);
        }
    });

    std::thread consumer(
        [allocated_future = std::move(allocated_future), released_promise = std::move(released_promise)]() mutable {
            auto allocated = allocated_future.get();
            auto& pool = ThreadLocalPool::local();

            for ( std::size_t i = 0; i < allocated.size(); ++i ) {
                SmallObject* ptr = allocated[i];
                check(ptr != nullptr, "consumer received null object");
                check(ptr->value == i, "consumer observed corrupted object value");
                check(ptr->payload[i % ptr->payload.size()] == static_cast<std::uint8_t>(i),
                    "consumer observed corrupted object payload");
                pool.destroy(ptr);
            }

            released_promise.set_value();
        });

    producer.join();
    consumer.join();

    const AddressList original_addresses = original_addresses_future.get();
    const AddressList recycled_addresses = recycled_addresses_future.get();

    check(original_addresses == recycled_addresses, "remote frees were not recycled back to the owner thread");
    return 0;
}
