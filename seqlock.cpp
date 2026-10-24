#include "seqlock.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <shared_mutex>
#include <mutex>
#include <cstdint>
#include <cassert>
#include <iomanip>
// #include <x86intrin.h> // For __rdtsc() on x86/x64

// Market Data Quote Payload (32-byte aligned struct)
struct alignas(32) MarketQuote {
    double bid;
    double ask;
    uint32_t bq;
    uint32_t aq;

    // Strict invariant enforced by writer: ask == bid + 0.05 AND aq == bq * 10
    bool is_valid() const noexcept {
        double diff = ask - bid;
        bool price_ok = (diff > 0.0499) && (diff < 0.0501);
        bool size_ok = (aq == bq * 10);
        return price_ok && size_ok;
    }
};

// Initial Unoptimized/Redundant Fenced SeqLock (Before Version)
template <typename T>
struct SeqLockBefore {
    alignas(64) std::atomic<uint32_t> seq{0};
    alignas(64) T value{};

    void write(const T& v) noexcept {
        uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        value = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2, std::memory_order_release);
    }

    bool read(T& out) const noexcept {
        uint32_t s0 = seq.load(std::memory_order_acquire);
        if (s0 & 1) return false;
        std::atomic_thread_fence(std::memory_order_acquire); // Redundant fence
        out = value;
        std::atomic_thread_fence(std::memory_order_acquire); // Redundant fence
        uint32_t s1 = seq.load(std::memory_order_relaxed);
        return (s0 == s1);
    }
};

// Shared Mutex Baseline Implementation
template <typename T>
struct SharedMutexLock {
    mutable std::shared_mutex rw_lock;
    alignas(64) T value{};

    void write(const T& v) {
        std::unique_lock<std::shared_mutex> lk(rw_lock);
        value = v;
    }

    bool read(T& out) const {
        std::shared_lock<std::shared_mutex> lk(rw_lock);
        out = value;
        return true;
    }
};

// Cycle counter helper function
inline uint64_t read_cycles() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
#endif
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "   SeqLock Optimization Benchmark & Correctness Verification    \n";
    std::cout << "=================================================================\n\n";

    constexpr int NUM_READERS = 4;
    constexpr auto DURATION = std::chrono::seconds(2);

    // ------------------------------------------------------------------------
    // Part 1: Correctness Test (1 Writer + 4 Readers under high contention)
    // ------------------------------------------------------------------------
    std::cout << "[1] Correctness Verification (1 Writer + " << NUM_READERS << " Readers)...\n";
    {
        SeqLock<MarketQuote> lock;
        lock.write({100.00, 100.05, 50, 500});

        std::atomic<bool> running{true};
        std::atomic<uint64_t> valid_reads{0};
        std::atomic<uint64_t> retries{0};
        std::atomic<uint64_t> corrupted_reads{0};

        std::thread writer([&]() {
            uint64_t seq = 1;
            while (running.load(std::memory_order_relaxed)) {
                double b = 100.0 + (seq % 1000) * 0.01;
                uint32_t bq = static_cast<uint32_t>(seq % 500 + 1);
                lock.write({b, b + 0.05, bq, bq * 10});
                seq++;
            }
        });

        std::vector<std::thread> readers;
        for (int r = 0; r < NUM_READERS; ++r) {
            readers.emplace_back([&]() {
                MarketQuote q{};
                uint64_t v_count = 0, r_count = 0, c_count = 0;
                while (running.load(std::memory_order_relaxed)) {
                    if (lock.read(q)) {
                        if (q.is_valid()) v_count++;
                        else c_count++;
                    } else {
                        r_count++;
                    }
                }
                valid_reads += v_count;
                retries += r_count;
                corrupted_reads += c_count;
            });
        }

        std::this_thread::sleep_for(DURATION);
        running.store(false, std::memory_order_relaxed);
        writer.join();
        for (auto& t : readers) t.join();

        std::cout << "    - Successful Untorn Reads : " << valid_reads.load() << "\n";
        std::cout << "    - Retries Detected        : " << retries.load() << "\n";
        std::cout << "    - Corrupted Reads         : " << corrupted_reads.load() << "\n";
        assert(corrupted_reads.load() == 0 && "CRITICAL BUG: Corrupted read detected!");
        std::cout << "    -> VERIFICATION PASSED: ZERO corrupted reads accepted.\n\n";
    }

    // ------------------------------------------------------------------------
    // Part 2: Latency Benchmark (Cycles / Operation)
    // ------------------------------------------------------------------------
    std::cout << "[2] Performance Benchmark (Cycles per Read Operation):\n";

    // A) Baseline: std::shared_mutex
    double cycles_mutex = 0.0, mops_mutex = 0.0;
    {
        SharedMutexLock<MarketQuote> lock;
        lock.write({100.0, 100.05, 100, 1000});
        std::atomic<bool> running{true};

        std::thread writer([&]() {
            uint64_t seq = 1;
            while (running.load(std::memory_order_relaxed)) {
                lock.write({100.0 + (seq % 100), 100.05 + (seq % 100), 100, 1000});
                seq++;
            }
        });

        std::vector<uint64_t> reader_ops(NUM_READERS, 0), reader_cyc(NUM_READERS, 0);
        std::vector<std::thread> readers;

        for (int r = 0; r < NUM_READERS; ++r) {
            readers.emplace_back([&, r]() {
                MarketQuote q{};
                uint64_t ops = 0, start = read_cycles();
                while (running.load(std::memory_order_relaxed)) {
                    if (lock.read(q)) ops++;
                }
                uint64_t end = read_cycles();
                reader_ops[r] = ops; reader_cyc[r] = end - start;
            });
        }

        std::this_thread::sleep_for(DURATION);
        running.store(false, std::memory_order_relaxed);
        writer.join();
        for (auto& t : readers) t.join();

        uint64_t total_ops = 0, total_cyc = 0;
        for (int i = 0; i < NUM_READERS; ++i) { total_ops += reader_ops[i]; total_cyc += reader_cyc[i]; }
        cycles_mutex = static_cast<double>(total_cyc) / total_ops;
        mops_mutex = total_ops / 2.0 / 1e6;
    }

    // B) SeqLock Before (Redundant Memory Barriers)
    double cycles_before = 0.0, mops_before = 0.0;
    {
        SeqLockBefore<MarketQuote> lock;
        lock.write({100.0, 100.05, 100, 1000});
        std::atomic<bool> running{true};

        std::thread writer([&]() {
            uint64_t seq = 1;
            while (running.load(std::memory_order_relaxed)) {
                lock.write({100.0 + (seq % 100), 100.05 + (seq % 100), 100, 1000});
                seq++;
            }
        });

        std::vector<uint64_t> reader_ops(NUM_READERS, 0), reader_cyc(NUM_READERS, 0);
        std::vector<std::thread> readers;

        for (int r = 0; r < NUM_READERS; ++r) {
            readers.emplace_back([&, r]() {
                MarketQuote q{};
                uint64_t ops = 0, start = read_cycles();
                while (running.load(std::memory_order_relaxed)) {
                    if (lock.read(q)) ops++;
                }
                uint64_t end = read_cycles();
                reader_ops[r] = ops; reader_cyc[r] = end - start;
            });
        }

        std::this_thread::sleep_for(DURATION);
        running.store(false, std::memory_order_relaxed);
        writer.join();
        for (auto& t : readers) t.join();

        uint64_t total_ops = 0, total_cyc = 0;
        for (int i = 0; i < NUM_READERS; ++i) { total_ops += reader_ops[i]; total_cyc += reader_cyc[i]; }
        cycles_before = static_cast<double>(total_cyc) / total_ops;
        mops_before = total_ops / 2.0 / 1e6;
    }

    // C) SeqLock After (Optimized Barrier Topology & Branch Hints)
    double cycles_after = 0.0, mops_after = 0.0;
    {
        SeqLock<MarketQuote> lock;
        lock.write({100.0, 100.05, 100, 1000});
        std::atomic<bool> running{true};

        std::thread writer([&]() {
            uint64_t seq = 1;
            while (running.load(std::memory_order_relaxed)) {
                lock.write({100.0 + (seq % 100), 100.05 + (seq % 100), 100, 1000});
                seq++;
            }
        });

        std::vector<uint64_t> reader_ops(NUM_READERS, 0), reader_cyc(NUM_READERS, 0);
        std::vector<std::thread> readers;

        for (int r = 0; r < NUM_READERS; ++r) {
            readers.emplace_back([&, r]() {
                MarketQuote q{};
                uint64_t ops = 0, start = read_cycles();
                while (running.load(std::memory_order_relaxed)) {
                    if (lock.read(q)) ops++;
                }
                uint64_t end = read_cycles();
                reader_ops[r] = ops; reader_cyc[r] = end - start;
            });
        }

        std::this_thread::sleep_for(DURATION);
        running.store(false, std::memory_order_relaxed);
        writer.join();
        for (auto& t : readers) t.join();

        uint64_t total_ops = 0, total_cyc = 0;
        for (int i = 0; i < NUM_READERS; ++i) { total_ops += reader_ops[i]; total_cyc += reader_cyc[i]; }
        cycles_after = static_cast<double>(total_cyc) / total_ops;
        mops_after = total_ops / 2.0 / 1e6;
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "    1) std::shared_mutex (Baseline) : " << std::setw(6) << cycles_mutex << " cycles/op  (" << mops_mutex << " M ops/sec)\n";
    std::cout << "    2) SeqLock (Before Revision)    : " << std::setw(6) << cycles_before << " cycles/op  (" << mops_before << " M ops/sec)\n";
    std::cout << "    3) SeqLock (After Revision)     : " << std::setw(6) << cycles_after << " cycles/op  (" << mops_after << " M ops/sec)\n\n";

    std::cout << "    [Key Metrics]\n";
    std::cout << "    - Speedup vs std::shared_mutex Baseline : " << (cycles_mutex / cycles_after) << "x faster\n";
    std::cout << "    - Latency Improvement vs Unoptimized    : " << ((cycles_before - cycles_after) / cycles_before * 100.0) << "% cycle reduction\n";
    std::cout << "=================================================================\n";

    return 0;
}