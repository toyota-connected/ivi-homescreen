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

#include <string_view>
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
  wake_cv_.notify_all();
  // running_ is already false, so any thread blocked in flush() wakes and
  // returns; the worker's final drain below flushes their records regardless.
  flush_done_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Worker::flush() noexcept {
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_.load(std::memory_order_acquire)) {
    return;  // no drain thread running; nothing to wait for
  }
  // Publish a new request and block until the worker reports it drained +
  // flushed a request at least this new. A record pushed before this call is
  // in its ring, so the drain the worker runs for this request captures it.
  const std::uint64_t target =
      flush_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
  wake_cv_.notify_all();
  flush_done_cv_.wait(lock, [&] {
    return flush_done_.load(std::memory_order_acquire) >= target ||
           !running_.load(std::memory_order_acquire);
  });
}

void Worker::run(ihs::stop_token stop) {
  int empty_polls = 0;
  drain_all();  // immediate first pass so startup logs are not held

  while (!stop.stop_requested()) {
    const auto interval =
        empty_polls >= kEmptyPollsBeforeIdle ? kIdleInterval : kActiveInterval;
    {
      std::unique_lock<std::mutex> lock(mu_);
      wake_cv_.wait_for(lock, interval, [&] {
        return stop.stop_requested() ||
               flush_request_.load(std::memory_order_acquire) >
                   flush_done_.load(std::memory_order_acquire);
      });
    }
    // Snapshot the request count BEFORE draining, so flush_done_ only advances
    // for records the drain below actually swept. A flush() that arrives during
    // the drain bumps flush_request_ past this snapshot and is satisfied on the
    // next iteration.
    const std::uint64_t req = flush_request_.load(std::memory_order_acquire);
    empty_polls = (drain_all() == 0) ? empty_polls + 1 : 0;
    // Only force the sinks' buffers on an explicit flush() / stop(); between
    // those, stdio buffering covers the periodic drains.
    if (req > flush_done_.load(std::memory_order_relaxed)) {
      flush_sinks();
      {
        std::lock_guard<std::mutex> lock(mu_);
        flush_done_.store(req, std::memory_order_release);
      }
      flush_done_cv_.notify_all();
    }
  }
  drain_all();
  flush_sinks();
  // Release any flusher still blocked as the worker exits.
  {
    std::lock_guard<std::mutex> lock(mu_);
    flush_done_.store(flush_request_.load(std::memory_order_acquire),
                      std::memory_order_release);
  }
  flush_done_cv_.notify_all();
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
        // Rejoin a message that was split across slots. The pieces are
        // consecutive in this ring -- one thread owns it, and the split loop
        // pushes them back to back -- but a drain can land mid-sequence, so
        // the partial lives on the ring across passes rather than in a local.
        // A sink is only ever handed a whole message, so it keeps printing one
        // prefix per line and needs to know nothing about any of this.
        const bool continues = (slot->flags & kFlagContinues) != 0;
        const bool joining = continues || !ring->partial().empty();
        if (joining) {
          if (ring->partial().empty()) {
            ring->begin_partial(slot->ts_ns);
          }
          ring->append_partial(slot->text, slot->text_len);
          if (continues) {
            ring->pop();
            ++drained;
            continue;
          }
        }
        const std::string_view whole =
            joining ? std::string_view{ring->partial()}
                    : std::string_view{slot->text, slot->text_len};
        const LogRecord record{
            entry->id.c_str(),
            &entry->dlt_ctx,
            static_cast<LogLevel>(slot->level),
            whole.data(),
            whole.size(),
            // A rejoined message is stamped when its first piece was pushed,
            // so it sorts where it was logged rather than where it finished.
            joining ? ring->partial_ts_ns() : slot->ts_ns,
        };
        for (const auto& sink : *sinks_) {
          sink->write(record);
        }
        ring->clear_partial();
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
