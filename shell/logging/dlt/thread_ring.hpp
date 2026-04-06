// shell/logging/dlt/thread_ring.hpp
#pragma once

#include "ring_slot.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ihs::dlt {

inline constexpr std::size_t kRingCapacity = 256; // power of two
static_assert((kRingCapacity & (kRingCapacity - 1)) == 0,
              "kRingCapacity must be a power of two");

// Single-producer (owning thread), single-consumer (worker) ring.
// Producer writes are lock-free and wait-free; the consumer drains by peeking
// and advancing the tail. Overflows are counted, not blocked.
class ThreadRing {
public:
    ThreadRing() noexcept = default;

    ThreadRing(const ThreadRing&)            = delete;
    ThreadRing& operator=(const ThreadRing&) = delete;

    // Producer side — called only from the ring's owning thread.
    bool push(std::uint32_t ctx_index,
              std::uint8_t  level,
              const char*   text,
              std::size_t   len) noexcept;

    // Consumer side — called only from the worker thread.
    [[nodiscard]] const RingSlot* peek() const noexcept;
    void pop() noexcept;

    [[nodiscard]] bool        empty()   const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    // Registry linkage — touched only under RingRegistry's ownership.
    ThreadRing* next = nullptr;

private:
    static constexpr std::uint32_t kMask = kRingCapacity - 1;

    alignas(64) std::atomic<std::uint32_t> head_{0}; // producer writes
    alignas(64) std::atomic<std::uint32_t> tail_{0}; // consumer reads
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
    alignas(64) RingSlot slots_[kRingCapacity]{};
};

} // namespace ihs::dlt
