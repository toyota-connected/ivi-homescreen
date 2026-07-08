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

ThreadRing& RingRegistry::acquire_ring() {
  // Reuse a ring released by an exited thread if one is available.
  for (ThreadRing* r = head_.load(std::memory_order_acquire); r != nullptr;
       r = r->next) {
    if (r->try_claim()) {
      return *r;
    }
  }
  // Pool exhausted (every ring is currently owned) — grow it by one. The new
  // ring is born claimed, so no other thread can take it before we return.
  auto* r = new ThreadRing();
  register_ring(r);
  return *r;
}

ThreadRing& RingRegistry::thread_local_ring() {
  // The lease releases the ring back to the pool at thread exit. The ring
  // itself is never freed (the worker may still be draining it); release()
  // only touches the ring's own atomic, so it is safe even at process exit
  // after this registry singleton is gone.
  struct Lease {
    ThreadRing* ring;
    ~Lease() {
      if (ring != nullptr) {
        ring->release();
      }
    }
  };
  static thread_local Lease lease{&RingRegistry::instance().acquire_ring()};
  return *lease.ring;
}

}  // namespace ihs::dlt
