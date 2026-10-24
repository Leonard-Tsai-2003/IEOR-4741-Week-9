#ifndef SEQLOCK_H
#define SEQLOCK_H

#include <atomic>
#include <cstdint>
#include <type_traits>

/**
 * @brief Lock-Free Sequence Lock (SeqLock) Template Class.
 * 
 * Optimized for single-writer, multi-reader concurrent architectures requiring
 * ultra-low latency updates (e.g., market data order book ticks, telemetry).
 *
 * Requirements on T:
 * - Must be TriviallyCopyable to guarantee safe bitwise assignment.
 */
template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable<T>::value, 
                  "SeqLock payload type T must be trivially copyable.");

private:
    // Separate sequence counter and payload onto distinct cache lines (64 bytes)
    // to eliminate false sharing between writing core and reading cores.
    alignas(64) std::atomic<uint32_t> seq_{0};
    alignas(64) T value_{};

public:
    SeqLock() noexcept = default;

    /**
     * @brief Writes a new payload value. Single-writer method.
     * @param v New payload to publish.
     */
    void write(const T& v) noexcept {
        uint32_t s = seq_.load(std::memory_order_relaxed);
        
        // 1. Mark sequence odd (signals write in progress to readers)
        seq_.store(s + 1, std::memory_order_relaxed);
        
        // Release fence guarantees sequence store completes before payload update
        std::atomic_thread_fence(std::memory_order_release);

        // 2. Publish payload update (may be read torn by concurrent readers)
        value_ = v;

        // 3. Mark sequence even with release semantics
        // Guarantees all payload updates complete before advancing sequence counter
        seq_.store(s + 2, std::memory_order_release);
    }

    /**
     * @brief Lock-free attempt to read the payload value.
     * @param out Reference to populate if read succeeds.
     * @return true if read was untorn and consistent; false if retry is needed.
     */
    bool read(T& out) const noexcept {
        // 1. Acquire load initial sequence counter
        uint32_t s0 = seq_.load(std::memory_order_acquire);

        // If sequence counter is odd, a writer is actively modifying the payload
        if (__builtin_expect(s0 & 1, 0)) {
            return false;
        }

        // 2. Read payload struct directly to stack
        out = value_;

        // Acquire fence prevents ending sequence load (s1) from being speculatively
        // reordered prior to payload copy completion
        std::atomic_thread_fence(std::memory_order_acquire);

        // 3. Load ending sequence counter
        uint32_t s1 = seq_.load(std::memory_order_relaxed);

        // Valid read only if sequence counter remained identical and even
        return __builtin_expect(s0 == s1, 1);
    }
};

#endif // SEQLOCK_H