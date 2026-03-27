#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <mcqnet/mcqnet.h>

namespace {
using clock_type = std::chrono::steady_clock;

volatile std::uint64_t benchmark_sink = 0;

struct BenchmarkConfig {
    std::size_t samples { 5 };
    std::size_t round_trip_pairs { 2'000'000 };
    std::size_t batch_size { 32'768 };
    std::size_t batch_rounds { 64 };
};

template <std::size_t ObjectSize> struct SmallObject {
    static_assert(ObjectSize >= sizeof(std::uint64_t));

    std::uint64_t value { 0 };
    std::array<std::uint8_t, ObjectSize - sizeof(std::uint64_t)> payload { };

    inline void touch(std::uint64_t seed) noexcept {
        value = seed + 1u;
        payload[seed % payload.size()] = static_cast<std::uint8_t>(seed);
    }
};

struct TimingResult {
    double median_ns { 0.0 };
    std::uint64_t checksum { 0 };
};

template <typename T> inline void do_not_optimize(const T& value) noexcept {
#if MCQNET_COMPILER_CLANG || MCQNET_COMPILER_GCC
    asm volatile("" : : "g"(value) : "memory");
#else
    (void)value;
#endif
}

inline void clobber_memory() noexcept {
#if MCQNET_COMPILER_CLANG || MCQNET_COMPILER_GCC
    asm volatile("" : : : "memory");
#endif
}

template <typename T> MCQNET_NODISCARD MCQNET_NOINLINE T* allocate_with_new() { return new T(); }

template <typename T> MCQNET_NOINLINE void deallocate_with_delete(T* ptr) { delete ptr; }

template <typename T> MCQNET_NODISCARD MCQNET_NOINLINE T* allocate_with_pool(mcqnet::memory::ObjectPool<T>& pool) {
    return pool.create();
}

template <typename T> MCQNET_NOINLINE void deallocate_with_pool(mcqnet::memory::ObjectPool<T>& pool, T* ptr) {
    pool.destroy(ptr);
}

MCQNET_NODISCARD inline std::uint64_t parse_positive(std::string_view value, const char* flag_name) {
    std::size_t parsed = 0;
    const std::string text { value };
    unsigned long long result = 0;
    try {
        result = std::stoull(text, &parsed);
    } catch ( const std::exception& ) {
        std::cerr << "invalid value for " << flag_name << ": " << text << '\n';
        std::exit(1);
    }

    if ( parsed != text.size() || result == 0 ) {
        std::cerr << "invalid value for " << flag_name << ": " << text << '\n';
        std::exit(1);
    }

    return static_cast<std::uint64_t>(result);
}

inline BenchmarkConfig parse_args(int argc, char** argv) {
    BenchmarkConfig config;

    for ( int i = 1; i < argc; ++i ) {
        const std::string_view arg { argv[i] };
        if ( arg == "--help" ) {
            std::cout << "Usage: benchmark_small_object_alloc [options]\n"
                      << "  --samples=N          measurement samples per case (default: 5)\n"
                      << "  --round-trip=N       alloc/free pairs for round-trip case (default: 2000000)\n"
                      << "  --batch-size=N       outstanding objects per batch (default: 32768)\n"
                      << "  --batch-rounds=N     batch repetitions (default: 64)\n";
            std::exit(0);
        }
        if ( arg.starts_with("--samples=") ) {
            config.samples = parse_positive(arg.substr(sizeof("--samples=") - 1u), "--samples");
            continue;
        }
        if ( arg.starts_with("--round-trip=") ) {
            config.round_trip_pairs = parse_positive(arg.substr(sizeof("--round-trip=") - 1u), "--round-trip");
            continue;
        }
        if ( arg.starts_with("--batch-size=") ) {
            config.batch_size = parse_positive(arg.substr(sizeof("--batch-size=") - 1u), "--batch-size");
            continue;
        }
        if ( arg.starts_with("--batch-rounds=") ) {
            config.batch_rounds = parse_positive(arg.substr(sizeof("--batch-rounds=") - 1u), "--batch-rounds");
            continue;
        }

        std::cerr << "unknown argument: " << arg << '\n';
        std::exit(1);
    }

    return config;
}

template <typename Fn> MCQNET_NODISCARD TimingResult measure_median(std::size_t samples, Fn&& fn) {
    benchmark_sink ^= fn();

    std::vector<double> timings;
    timings.reserve(samples);

    std::uint64_t checksum = 0;
    for ( std::size_t i = 0; i < samples; ++i ) {
        const auto start = clock_type::now();
        const std::uint64_t value = fn();
        const auto stop = clock_type::now();

        checksum ^= value + static_cast<std::uint64_t>(i + 1u);
        benchmark_sink ^= value;
        timings.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
    }

    std::sort(timings.begin(), timings.end());
    return { timings[timings.size() / 2u], checksum };
}

template <typename T> MCQNET_NODISCARD std::uint64_t run_new_delete_round_trip(std::size_t pairs) {
    std::uint64_t checksum = 0;
    for ( std::size_t i = 0; i < pairs; ++i ) {
        T* ptr = allocate_with_new<T>();
        do_not_optimize(ptr);
        ptr->touch(i);
        clobber_memory();
        checksum += ptr->value;
        checksum += ptr->payload[i % ptr->payload.size()];
        deallocate_with_delete(ptr);
    }
    return checksum;
}

template <typename T>
MCQNET_NODISCARD std::uint64_t run_object_pool_round_trip(mcqnet::memory::ObjectPool<T>& pool, std::size_t pairs) {
    std::uint64_t checksum = 0;
    for ( std::size_t i = 0; i < pairs; ++i ) {
        T* ptr = allocate_with_pool(pool);
        do_not_optimize(ptr);
        ptr->touch(i);
        clobber_memory();
        checksum += ptr->value;
        checksum += ptr->payload[i % ptr->payload.size()];
        deallocate_with_pool(pool, ptr);
    }
    return checksum;
}

template <typename T> MCQNET_NODISCARD std::uint64_t run_new_delete_batch(std::size_t batch_size, std::size_t rounds) {
    std::vector<T*> objects(batch_size, nullptr);
    std::uint64_t checksum = 0;

    for ( std::size_t round = 0; round < rounds; ++round ) {
        for ( std::size_t i = 0; i < batch_size; ++i ) {
            const std::size_t index = round * batch_size + i;
            T* ptr = allocate_with_new<T>();
            ptr->touch(index);
            do_not_optimize(ptr);
            objects[i] = ptr;
        }
        for ( std::size_t i = 0; i < batch_size; ++i ) {
            T* ptr = objects[i];
            checksum += ptr->value;
            checksum += ptr->payload[i % ptr->payload.size()];
            deallocate_with_delete(ptr);
        }
    }

    return checksum;
}

template <typename T>
MCQNET_NODISCARD std::uint64_t run_object_pool_batch(
    mcqnet::memory::ObjectPool<T>& pool, std::size_t batch_size, std::size_t rounds) {
    std::vector<T*> objects(batch_size, nullptr);
    std::uint64_t checksum = 0;

    for ( std::size_t round = 0; round < rounds; ++round ) {
        for ( std::size_t i = 0; i < batch_size; ++i ) {
            const std::size_t index = round * batch_size + i;
            T* ptr = allocate_with_pool(pool);
            ptr->touch(index);
            do_not_optimize(ptr);
            objects[i] = ptr;
        }
        for ( std::size_t i = 0; i < batch_size; ++i ) {
            T* ptr = objects[i];
            checksum += ptr->value;
            checksum += ptr->payload[i % ptr->payload.size()];
            deallocate_with_pool(pool, ptr);
        }
    }

    return checksum;
}

inline void print_header(const BenchmarkConfig& config) {
    const std::size_t batch_pairs = config.batch_size * config.batch_rounds;

    std::cout << "Small object allocation benchmark\n";
    std::cout << "samples=" << config.samples << ", round_trip_pairs=" << config.round_trip_pairs
              << ", batch_pairs=" << batch_pairs << " (" << config.batch_rounds << " x " << config.batch_size
              << ")\n\n";

    std::cout << std::left << std::setw(12) << "scenario" << std::setw(14) << "object_size" << std::setw(18)
              << "new/delete(ns/op)" << std::setw(18) << "pool(ns/op)" << std::setw(12) << "speedup" << '\n';
    std::cout << std::string(74, '-') << '\n';
}

template <typename T, typename NewDeleteFn, typename PoolFn>
void print_case(std::string_view scenario, std::size_t samples, std::size_t operations, NewDeleteFn&& new_delete_fn,
    PoolFn&& pool_fn) {
    mcqnet::memory::ObjectPool<T> pool;

    const TimingResult new_delete = measure_median(samples, [&]() { return new_delete_fn(); });
    const TimingResult object_pool = measure_median(samples, [&]() { return pool_fn(pool); });

    const double new_delete_per_op = new_delete.median_ns / static_cast<double>(operations);
    const double object_pool_per_op = object_pool.median_ns / static_cast<double>(operations);
    const double speedup = new_delete_per_op / object_pool_per_op;

    std::cout << std::left << std::setw(12) << scenario << std::setw(14) << sizeof(T) << std::setw(18) << std::fixed
              << std::setprecision(2) << new_delete_per_op << std::setw(18) << object_pool_per_op << std::setw(12)
              << speedup << '\n';
}

template <std::size_t ObjectSize> void run_object_size_suite(const BenchmarkConfig& config) {
    using Object = SmallObject<ObjectSize>;

    print_case<Object>(
        "round-trip", config.samples, config.round_trip_pairs,
        [&]() { return run_new_delete_round_trip<Object>(config.round_trip_pairs); },
        [&](mcqnet::memory::ObjectPool<Object>& pool) {
            return run_object_pool_round_trip<Object>(pool, config.round_trip_pairs);
        });

    print_case<Object>(
        "batched", config.samples, config.batch_size * config.batch_rounds,
        [&]() { return run_new_delete_batch<Object>(config.batch_size, config.batch_rounds); },
        [&](mcqnet::memory::ObjectPool<Object>& pool) {
            return run_object_pool_batch<Object>(pool, config.batch_size, config.batch_rounds);
        });
}
} // namespace

int main(int argc, char** argv) {
    const BenchmarkConfig config = parse_args(argc, argv);

    print_header(config);
    run_object_size_suite<16>(config);
    run_object_size_suite<64>(config);
    run_object_size_suite<256>(config);

    std::cout << "\nbenchmark_sink=" << benchmark_sink << '\n';
    return 0;
}
