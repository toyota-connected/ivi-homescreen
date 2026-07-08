// shell/logging/dlt/worker.cpp
#include "worker.hpp"

#include "context_cache.hpp"
#include "libdlt_loader.hpp"
#include "ring_registry.hpp"
#include "ring_slot.hpp"
#include "thread_ring.hpp"

namespace ihs::dlt {

Worker::Worker(RingRegistry& registry,
               ContextCache& cache,
               LibDltLoader& loader) noexcept
    : registry_(registry), cache_(cache), loader_(loader) {}

Worker::~Worker() {
  stop();
}

void Worker::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  thread_ = ihs::jthread([this](ihs::stop_token st) { run(std::move(st)); });
}

void Worker::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  thread_.request_stop();
  {
    std::lock_guard<std::mutex> lock(mu_);
    flush_requested_.store(true, std::memory_order_release);
  }
  wake_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Worker::flush() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    flush_requested_.store(true, std::memory_order_release);
  }
  wake_cv_.notify_all();
}

void Worker::run(ihs::stop_token stop) {
  while (!stop.stop_requested()) {
    drain_all();
    flush_requested_.store(false, std::memory_order_release);

    std::unique_lock<std::mutex> lock(mu_);
    wake_cv_.wait_for(lock, kInterval, [&] {
      return stop.stop_requested() ||
             flush_requested_.load(std::memory_order_acquire) ||
             RingRegistry::instance().any_immediate();
    });
  }
  drain_all();
}

void Worker::drain_all() {
  for (ThreadRing* ring = registry_.head(); ring != nullptr;
       ring = ring->next) {
    while (const RingSlot* slot = ring->peek()) {
      if (ContextEntry* entry = cache_.at(slot->ctx_index)) {
        loader_.emit(&entry->dlt_ctx, static_cast<int>(slot->level),
                     slot->text);
      }
      ring->pop();
    }
  }
}

}  // namespace ihs::dlt
