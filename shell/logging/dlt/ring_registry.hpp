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

  // Returns a reference to the calling thread's ring, creating it on first
  // use. Thread-local storage owns the lifetime — rings leak at process
  // exit, which is intentional (the worker may still be draining).
  ThreadRing& thread_local_ring();

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

  std::atomic<ThreadRing*> head_{nullptr};
};

}  // namespace ihs::dlt
