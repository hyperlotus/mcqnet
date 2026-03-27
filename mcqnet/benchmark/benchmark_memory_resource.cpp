#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include <mcqnet/detail/macro.h>
#include <mcqnet/memory/memory_resource.h>

namespace {
using clock_type = std::chrono::steady_clock;

volatile std::uint64_t benchmark_sink = 0;

struct BenchmarkConfig {
    std::size_t samples { 5 };
    std::size_t round_trip_ops { 2'000'000 };
    std::size_t batch_size { 32'768 };
    std::size_t batch_rounds { 64 };
};

struct AllocationCase {
    std::string_view label;
    std::size_t bytes;
    std::size_t alignment;
};

struct TimingResult {
    double median_ns { 0.0 };
    std::uint64_t checksum { 0 };
};

inline constexpr std::array<AllocationCase, 6> allocation_cases
    = { { { "small-16", 16, alignof(std::max_align_t) }, { "small-64", 64, alignof(std::max_align_t) },
        { "small-256", 256, alignof(std::max_align_t) }, { "edge-4k", 4096, alignof(std::max_align_t) },
        { "large-8k", 8192, alignof(std::max_align_t) }, { "align-64", 64, 64 } } };

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
            std::cout << "Usage: benchmark_memory_resource [options]\n"
                      << "  --samples=N          measurement samples per case (default: 5)\n"
                      << "  --round-trip=N       alloc/free pairs per case (default: 2000000)\n"
                      << "  --batch-size=N       outstanding allocations per batch (default: 32768)\n"
                      << "  --batch-rounds=N     batch repetitions per case (default: 64)\n";
            std::exit(0);
        }
        if ( arg.starts_with("--samples=") ) {
            config.samples = parse_positive(arg.substr(sizeof("--samples=") - 1u), "--samples");
            continue;
        }
        if ( arg.starts_with("--round-trip=") ) {
            config.round_trip_ops = parse_positive(arg.substr(sizeof("--round-trip=") - 1u), "--round-trip");
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

inline std::uint64_t touch_bytes(void* ptr, std::size_t bytes, std::uint64_t seed) noexcept {
    auto* data = static_cast<std::byte*>(ptr);
    const std::size_t last_index = bytes - 1u;

    data[0] = static_cast<std::byte>(seed & 0xFFu);
    data[last_index] = static_cast<std::byte>((seed >> 8u) & 0xFFu);
    clobber_memory();

    return static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[0]))
        + static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[last_index]));
}

MCQNET_NODISCARD std::uint64_t run_round_trip(
    std::pmr::memory_resource& resource, std::size_t operations, std::size_t bytes, std::size_t alignment) {
    std::uint64_t checksum = 0;

    for ( std::size_t i = 0; i < operations; ++i ) {
        void* ptr = resource.allocate(bytes, alignment);
        do_not_optimize(ptr);
        checksum += touch_bytes(ptr, bytes, i);
        resource.deallocate(ptr, bytes, alignment);
    }

    return checksum;
}

MCQNET_NODISCARD std::uint64_t run_batched(std::pmr::memory_resource& resource, std::size_t batch_size,
    std::size_t rounds, std::size_t bytes, std::size_t alignment) {
    std::vector<void*> allocations(batch_size, nullptr);
    std::uint64_t checksum = 0;

    for ( std::size_t round = 0; round < rounds; ++round ) {
        for ( std::size_t i = 0; i < batch_size; ++i ) {
            const std::size_t index = round * batch_size + i;
            void* ptr = resource.allocate(bytes, alignment);
            do_not_optimize(ptr);
            checksum += touch_bytes(ptr, bytes, index);
            allocations[i] = ptr;
        }

        for ( void* ptr : allocations ) {
            resource.deallocate(ptr, bytes, alignment);
        }
    }

    return checksum;
}

inline void print_header(const BenchmarkConfig& config) {
    const std::size_t batch_ops = config.batch_size * config.batch_rounds;

    std::cout << "PMR memory_resource benchmark\n";
    std::cout << "samples=" << config.samples << ", round_trip_ops=" << config.round_trip_ops
              << ", batch_ops=" << batch_ops << " (" << config.batch_rounds << " x " << config.batch_size << ")\n\n";

    std::cout << std::left << std::setw(12) << "case" << std::setw(12) << "scenario" << std::setw(12) << "bytes"
              << std::setw(12) << "align" << std::setw(20) << "std_new(ns/op)" << std::setw(20) << "std_pool(ns/op)"
              << std::setw(20) << "mcqnet(ns/op)" << std::setw(12) << "new/mcq" << std::setw(12) << "pool/mcq" << '\n';
    std::cout << std::string(132, '-') << '\n';
}

template <typename WorkloadFn>
void print_case(std::string_view case_label, std::string_view scenario, const AllocationCase& allocation_case,
    std::size_t samples, std::size_t operations, WorkloadFn&& workload_fn) {
    mcqnet::memory::MemoryResource mcqnet_resource;
    std::pmr::unsynchronized_pool_resource std_pool_resource;
    std::pmr::memory_resource* baseline_resource = std::pmr::new_delete_resource();

    const TimingResult baseline_new = measure_median(samples, [&]() { return workload_fn(*baseline_resource); });
    const TimingResult baseline_pool = measure_median(samples, [&]() { return workload_fn(std_pool_resource); });
    const TimingResult mcqnet = measure_median(samples, [&]() { return workload_fn(mcqnet_resource); });

    const double baseline_new_per_op = baseline_new.median_ns / static_cast<double>(operations);
    const double baseline_pool_per_op = baseline_pool.median_ns / static_cast<double>(operations);
    const double mcqnet_per_op = mcqnet.median_ns / static_cast<double>(operations);
    const double speedup_vs_new = baseline_new_per_op / mcqnet_per_op;
    const double speedup_vs_pool = baseline_pool_per_op / mcqnet_per_op;

    std::cout << std::left << std::setw(12) << case_label << std::setw(12) << scenario << std::setw(12)
              << allocation_case.bytes << std::setw(12) << allocation_case.alignment << std::setw(20) << std::fixed
              << std::setprecision(2) << baseline_new_per_op << std::setw(20) << baseline_pool_per_op << std::setw(20)
              << mcqnet_per_op << std::setw(12) << speedup_vs_new << std::setw(12) << speedup_vs_pool << '\n';
}

void run_case_suite(const BenchmarkConfig& config, const AllocationCase& allocation_case) {
    print_case(allocation_case.label, "round-trip", allocation_case, config.samples, config.round_trip_ops,
        [&](std::pmr::memory_resource& resource) {
            return run_round_trip(resource, config.round_trip_ops, allocation_case.bytes, allocation_case.alignment);
        });

    print_case(allocation_case.label, "batched", allocation_case, config.samples,
        config.batch_size * config.batch_rounds, [&](std::pmr::memory_resource& resource) {
            return run_batched(
                resource, config.batch_size, config.batch_rounds, allocation_case.bytes, allocation_case.alignment);
        });
}
} // namespace

int main(int argc, char** argv) {
    const BenchmarkConfig config = parse_args(argc, argv);

    print_header(config);
    for ( const AllocationCase& allocation_case : allocation_cases ) {
        run_case_suite(config, allocation_case);
    }

    std::cout << "\nbenchmark_sink=" << benchmark_sink << '\n';
    return 0;
}
