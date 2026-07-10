/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "worker.hpp"

#include "context_cache.hpp"
#include "ring_registry.hpp"
#include "ring_slot.hpp"
#include "thread_ring.hpp"

namespace ihs::dlt {

Worker::Worker(RingRegistry& registry, ContextCache& cache) noexcept
    : registry_(registry), cache_(cache) {}

Worker::~Worker() {
  stop();
}

void Worker::start(const std::vector<std::unique_ptr<Sink>>& sinks) {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  sinks_ = &sinks;
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
  int empty_polls = 0;
  drain_all();  // immediate first pass so startup logs are not held

  while (!stop.stop_requested()) {
    const auto interval =
        empty_polls >= kEmptyPollsBeforeIdle ? kIdleInterval : kActiveInterval;
    bool flushed = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      wake_cv_.wait_for(lock, interval, [&] {
        return stop.stop_requested() ||
               flush_requested_.load(std::memory_order_acquire);
      });
      // Consume the flush request under the lock, paired with flush()'s set
      // under the lock: a flush() arriving during the drain below sets it
      // again, so the next iteration re-drains and none is lost.
      flushed = flush_requested_.exchange(false, std::memory_order_acq_rel);
    }

    empty_polls = (drain_all() == 0) ? empty_polls + 1 : 0;
    // Only force the sinks' buffers on an explicit flush() / stop(); between
    // those, stdio buffering covers the periodic drains.
    if (flushed) {
      flush_sinks();
    }
  }
  drain_all();
  flush_sinks();
}

std::size_t Worker::drain_all() {
  if (sinks_ == nullptr) {
    return 0;
  }
  std::size_t drained = 0;
  for (ThreadRing* ring = registry_.head(); ring != nullptr;
       ring = ring->next) {
    while (const RingSlot* slot = ring->peek()) {
      if (ContextEntry* entry = cache_.at(slot->ctx_index)) {
        const LogRecord record{
            entry->id.c_str(),
            &entry->dlt_ctx,
            static_cast<LogLevel>(slot->level),
            slot->text,
            slot->text_len,
            slot->ts_ns,  // emit time, captured when the record was pushed
        };
        for (const auto& sink : *sinks_) {
          sink->write(record);
        }
      }
      ring->pop();
      ++drained;
    }
  }
  return drained;
}

void Worker::flush_sinks() noexcept {
  if (sinks_ == nullptr) {
    return;
  }
  for (const auto& sink : *sinks_) {
    sink->flush();
  }
}

}  // namespace ihs::dlt
