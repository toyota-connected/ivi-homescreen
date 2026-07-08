// shell/logging/dlt/ring_registry.cpp
#include "ring_registry.hpp"

namespace ihs::dlt {

RingRegistry& RingRegistry::instance() {
  static RingRegistry the_registry;
  return the_registry;
}

void RingRegistry::register_ring(ThreadRing* r) noexcept {
  ThreadRing* expected_head = head_.load(std::memory_order_acquire);
  do {
    r->next = expected_head;
  } while (!head_.compare_exchange_weak(
      expected_head, r, std::memory_order_release, std::memory_order_acquire));
}

ThreadRing& RingRegistry::thread_local_ring() {
  // Intentionally leaked at thread exit — the worker may still hold a
  // pointer while draining. A tombstone marker would be needed to reclaim.
  static thread_local ThreadRing* local_ring = [this] {
    auto* r = new ThreadRing();
    register_ring(r);
    return r;
  }();
  return *local_ring;
}

}  // namespace ihs::dlt
