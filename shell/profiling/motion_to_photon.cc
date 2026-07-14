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

#include "profiling/motion_to_photon.h"

#include <cstdlib>
#include <string>

#include <asio/post.hpp>

#include "logging/logging.h"
#include "task_runner.h"

namespace profiling {

bool MotionToPhoton::Enabled() {
  return std::getenv("IVI_PROFILE") != nullptr ||
         std::getenv("IVI_M2P_PROFILE") != nullptr;
}

void MotionToPhoton::Hist::Add(const uint64_t latency_ns) {
  const auto us = static_cast<uint32_t>(latency_ns / 1000ULL);
  const uint32_t bucket = us / 1000u;
  ms[bucket < kMaxMs ? bucket : kMaxMs]++;
  count++;
  sum_us += us;
  if (us > max_us) {
    max_us = us;
  }
}

void MotionToPhoton::Hist::Merge(const Hist& other) {
  for (uint32_t i = 0; i <= kMaxMs; ++i) {
    ms[i] += other.ms[i];
  }
  count += other.count;
  sum_us += other.sum_us;
  if (other.max_us > max_us) {
    max_us = other.max_us;
  }
}

uint32_t MotionToPhoton::Hist::Pct(const double p) const {
  if (count == 0) {
    return 0;
  }
  const auto target = static_cast<uint32_t>(p * count);
  uint32_t cum = 0;
  for (uint32_t i = 0; i <= kMaxMs; ++i) {
    cum += ms[i];
    if (cum >= target + 1) {
      // Bucket i covers [i, i+1) ms; report its midpoint in microseconds.
      return i * 1000u + 500u;
    }
  }
  return kMaxMs * 1000u;
}

void MotionToPhoton::RecordInput(const uint64_t input_ns) {
  const uint32_t next = (tail_ + 1u) % kPending;
  if (next == head_) {
    // Ring full — drop the oldest so the newest (most relevant) input is kept.
    // Keep floor_head_ >= head_: if the dropped entry was not yet floored,
    // advance floor_head_ past it too.
    if (floor_head_ == head_) {
      floor_head_ = (floor_head_ + 1u) % kPending;
    }
    head_ = (head_ + 1u) % kPending;
    dropped_++;
  }
  pending_[tail_] = input_ns;
  tail_ = next;
}

void MotionToPhoton::RecordPresent(const uint64_t present_ns,
                                   const uint64_t cutoff_ns,
                                   const std::string_view label) {
  // Floor pass: any queued input at or before this scanout could first be
  // visible now. Read-ahead only (floor_head_ does not remove) so the same
  // inputs remain for the frame-accurate pass.
  while (floor_head_ != tail_ && pending_[floor_head_] <= present_ns) {
    const uint64_t latency_ns = present_ns - pending_[floor_head_];
    window_floor_.Add(latency_ns);
    session_floor_.Add(latency_ns);
    floor_head_ = (floor_head_ + 1u) % kPending;
  }

  // Frame-accurate pass: this frame's build began at cutoff_ns, so it rendered
  // exactly the inputs at or before that cutoff; each gets present − input.
  // Inputs after the cutoff arrived too late for this frame — they stay queued.
  while (head_ != tail_ && pending_[head_] <= cutoff_ns) {
    const uint64_t latency_ns = present_ns - pending_[head_];
    window_.Add(latency_ns);
    session_.Add(latency_ns);
    head_ = (head_ + 1u) % kPending;
  }

  if (++presents_ % kWindow == 0 && window_.count > 0) {
    ihs::log::info(
        "[{}] motion->photon: frame-accurate p50={:.1f}ms p99={:.1f}ms "
        "max={:.1f}ms | floor p50={:.1f}ms p99={:.1f}ms | n={} dropped={}",
        label, window_.Pct(0.50) / 1000.0, window_.Pct(0.99) / 1000.0,
        window_.max_us / 1000.0, window_floor_.Pct(0.50) / 1000.0,
        window_floor_.Pct(0.99) / 1000.0, window_.count, dropped_);
    window_ = Hist{};
    window_floor_ = Hist{};
    dropped_ = 0;
  }
}

void MotionToPhoton::LogSessionSummary(const std::string_view label) const {
  if (session_.count == 0) {
    return;
  }
  ihs::log::info(
      "[{}] motion->photon session (frame-accurate, {} samples): mean={:.1f}ms "
      "p50={:.1f}ms p95={:.1f}ms p99={:.1f}ms max={:.1f}ms",
      label, session_.count,
      static_cast<double>(session_.sum_us) / session_.count / 1000.0,
      session_.Pct(0.50) / 1000.0, session_.Pct(0.95) / 1000.0,
      session_.Pct(0.99) / 1000.0, session_.max_us / 1000.0);
  if (session_floor_.count > 0) {
    ihs::log::info(
        "[{}] motion->photon session (floor, {} samples): mean={:.1f}ms "
        "p50={:.1f}ms p95={:.1f}ms p99={:.1f}ms max={:.1f}ms",
        label, session_floor_.count,
        static_cast<double>(session_floor_.sum_us) / session_floor_.count /
            1000.0,
        session_floor_.Pct(0.50) / 1000.0, session_floor_.Pct(0.95) / 1000.0,
        session_floor_.Pct(0.99) / 1000.0, session_floor_.max_us / 1000.0);
  }
}

void MarshalRecordPresent(TaskRunner* runner,
                          MotionToPhoton* m2p,
                          const uint64_t present_ns,
                          const uint64_t cutoff_ns,
                          const std::string_view label) {
  if (m2p == nullptr || runner == nullptr) {
    return;
  }
  // The label is a backend-static literal, but copy it into the task so the
  // contract holds regardless of caller lifetime.
  asio::post(*runner->GetStrandContext(),
             [m2p, present_ns, cutoff_ns, label = std::string(label)]() {
               m2p->RecordPresent(present_ns, cutoff_ns, label);
             });
}

}  // namespace profiling
