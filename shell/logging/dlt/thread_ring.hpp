// shell/logging/dlt/thread_ring.hpp
#pragma once

#include "ring_slot.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ihs::dlt {

inline constexpr std::size_t kRingCapacity = 256;  // power of two
static_assert((kRingCapacity & (kRingCapacity - 1)) == 0,
              "kRingCapacity must be a power of two");

// Single-producer (owning thread), single-consumer (worker) ring.
// Producer writes are lock-free and wait-free; the consumer drains by peeking
// and advancing the tail. Overflows are counted, not blocked.
class ThreadRing {
 public:
  ThreadRing() noexcept = default;

  ThreadRing(const ThreadRing&) = delete;
  ThreadRing& operator=(const ThreadRing&) = delete;

  // Producer side — called only from the ring's owning thread.
  bool push(std::uint32_t ctx_index,
            std::uint8_t level,
            const char* text,
            std::size_t len) noexcept;

  // Consumer side — called only from the worker thread.
  [[nodiscard]] const RingSlot* peek() const noexcept;
  void pop() noexcept;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t pending() const noexcept;

  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

  // Ownership pool (see RingRegistry). A ring is owned by exactly one producer
  // thread at a time. try_claim() takes an unowned ring; release() (run from
  // the owner's thread-exit) returns it to the pool for a later thread to
  // reuse, so total rings stay bounded by peak concurrent logging threads
  // rather than the count of threads that ever logged. A fresh ring is born
  // owned by its creator. The ring is never freed and stays registered, so
  // the worker keeps draining any residual slots across an ownership change;
  // SPSC holds because release() runs only after the old owner's last push.
  [[nodiscard]] bool try_claim() noexcept {
    bool expected = false;
    return in_use_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
  }
  void release() noexcept { in_use_.store(false, std::memory_order_release); }

  // Registry linkage — touched only under RingRegistry's ownership.
  ThreadRing* next = nullptr;

 private:
  static constexpr std::uint32_t kMask = kRingCapacity - 1;

  std::atomic<bool> in_use_{true};  // born claimed by its creating thread
  alignas(64) std::atomic<std::uint32_t> head_{0};  // producer writes
  alignas(64) std::atomic<std::uint32_t> tail_{0};  // consumer reads
  alignas(64) std::atomic<std::uint64_t> dropped_{0};
  alignas(64) RingSlot slots_[kRingCapacity]{};
};

}  // namespace ihs::dlt
