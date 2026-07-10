// shell/logging/dlt/ring_registry.hpp
#pragma once

#include "thread_ring.hpp"

#include <atomic>

namespace ihs::dlt {

// Intrusive lock-free list of ThreadRings, one per logging thread.
// The worker iterates head()->next->... to drain all rings.
class RingRegistry {
 public:
  static RingRegistry& instance();

  // Returns a reference to the calling thread's ring, claiming a released
  // one from the pool or creating a new one on first use. A thread_local
  // lease releases the ring back to the pool at thread exit so the total
  // ring count is bounded by peak concurrent logging threads. Rings are
  // never freed (the worker may still be draining), but they are reused, so
  // memory does not grow with the count of threads that have ever logged.
  //
  // Static: the ring is keyed off the calling thread (a thread_local lease)
  // and the pool is reached through instance(), so this never touches a
  // particular registry object.
  static ThreadRing& thread_local_ring();

  [[nodiscard]] ThreadRing* head() const noexcept {
    return head_.load(std::memory_order_acquire);
  }

  // Reserved for future immediate-flush hints. Always false today.
  [[nodiscard]] bool any_immediate() const noexcept { return false; }

 private:
  RingRegistry() = default;
  ~RingRegistry() = default;
  RingRegistry(const RingRegistry&) = delete;
  RingRegistry& operator=(const RingRegistry&) = delete;

  void register_ring(ThreadRing* r) noexcept;

  // Claim a released ring from the pool, or allocate+register a new one.
  ThreadRing& acquire_ring();

  std::atomic<ThreadRing*> head_{nullptr};
};

}  // namespace ihs::dlt
