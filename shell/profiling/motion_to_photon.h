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

#ifndef SHELL_PROFILING_MOTION_TO_PHOTON_H_
#define SHELL_PROFILING_MOTION_TO_PHOTON_H_

#include <array>
#include <cstdint>
#include <string_view>

namespace profiling {

/**
 * @brief Motion-to-photon (input-to-scanout) latency profiler.
 *
 * Joins two endpoints that both carry CLOCK_MONOTONIC timestamps: kernel input
 * time from zwp_input_timestamps_v1 (RecordInput, from the pointer/touch
 * handlers) and compositor scanout time from wp_presentation feedback
 * (RecordPresent, from the vsync provider's presented event).
 *
 * v1 metric — input-to-next-scanout: every input timestamp is queued, and each
 * present drains the inputs it is at-or-after, recording present − input for
 * each. This is the *floor* of true motion-to-photon: the scanout at
 * present_ns is the first moment an input could be visible, but the frame that
 * actually reflects a given input may present a cycle or two later (engine
 * pipeline depth). Frame-accurate correlation — pairing an input with the
 * frame that rendered it via the vsync baton cutoff — is a follow-up that
 * refines this into the true latency; the floor is already useful and moves
 * exactly with an injected delay.
 *
 * Enabled by IVI_M2P_PROFILE (or IVI_PROFILE). Threading: RecordInput and
 * RecordPresent are both called on the Wayland event thread, so no locking —
 * the same contract as the input-timestamps provider and the vsync feedback.
 */
class MotionToPhoton {
 public:
  /// True when IVI_M2P_PROFILE or IVI_PROFILE is set. Evaluate once and cache.
  [[nodiscard]] static bool Enabled();

  /// Queue a kernel input timestamp (nanoseconds, CLOCK_MONOTONIC).
  void RecordInput(uint64_t input_ns);

  /// A frame scanned out at @p present_ns (nanoseconds, CLOCK_MONOTONIC).
  /// Records present − input for every queued input at or before it. @p label
  /// tags the window log line (backend name).
  void RecordPresent(uint64_t present_ns, std::string_view label);

  /// Emit the session-aggregate percentiles. No-op when nothing was recorded.
  void LogSessionSummary(std::string_view label) const;

 private:
  // Latency histogram: one bucket per millisecond up to kMaxMs, plus an
  // overflow bucket. Percentiles are read from the cumulative counts — cheap,
  // allocation-free, and ±1 ms is finer than the measurement is meaningful.
  static constexpr uint32_t kMaxMs = 200;
  static constexpr uint32_t kWindow = 120;  // presents per window log

  struct Hist {
    std::array<uint32_t, kMaxMs + 1> ms{};  // [kMaxMs] = overflow (>= kMaxMs)
    uint32_t count{0};
    uint64_t sum_us{0};
    uint32_t max_us{0};

    void Add(uint64_t latency_ns);
    void Merge(const Hist& other);
    // Bucket-midpoint percentile in microseconds; 0 when empty.
    [[nodiscard]] uint32_t Pct(double p) const;
  };

  // FIFO ring of pending input timestamps (ns). Monotonic, so a present drains
  // from the front. Overflow (inputs with no interleaving present — a stall or
  // idle burst) drops the oldest and is counted.
  static constexpr uint32_t kPending = 256;
  std::array<uint64_t, kPending> pending_{};
  uint32_t head_{0};
  uint32_t tail_{0};
  uint32_t dropped_{0};

  Hist window_{};
  Hist session_{};
  uint32_t presents_{0};

  [[nodiscard]] bool Empty() const { return head_ == tail_; }
};

}  // namespace profiling

#endif  // SHELL_PROFILING_MOTION_TO_PHOTON_H_
