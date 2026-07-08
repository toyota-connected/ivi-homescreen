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
 * Two latencies are reported per present, from the same queued inputs:
 *
 *   - frame-accurate (primary): each present carries its frame's input *cutoff*
 *     — the frame_start_time of the baton that drove it (see
 *     IVsyncProvider::LastDeliveredFrameStartNs). An input at or before the
 *     cutoff was available when that frame's build began, so that frame is the
 *     one that rendered it; its latency is present − input. Inputs after the
 *     cutoff stay queued for the next frame. This is true motion-to-photon.
 *
 *   - floor (secondary): present − input for the first scanout at or after the
 *     input, regardless of which frame consumed it. The scanout is the earliest
 *     an input *could* be visible, so this under-counts by the engine pipeline
 *     depth. Reported alongside the frame-accurate number; their difference is
 *     that pipeline depth (typically one to two frames).
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

  /// A frame scanned out at @p present_ns (nanoseconds, CLOCK_MONOTONIC) whose
  /// input cutoff (frame_start_time) was @p cutoff_ns. Drains and records the
  /// frame-accurate latency for every queued input at or before the cutoff, and
  /// the floor latency for every input at or before @p present_ns. @p label
  /// tags the window log line (backend name). A @p cutoff_ns of 0 (unknown)
  /// drains nothing frame-accurately — those inputs roll to a later present.
  void RecordPresent(uint64_t present_ns,
                     uint64_t cutoff_ns,
                     std::string_view label);

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
  // from the front. Two cursors chase tail_: head_ removes entries at the frame
  // cutoff (frame-accurate); floor_head_ reads ahead to the present timestamp
  // (floor) without removing. floor_head_ is always at or ahead of head_ in
  // ring order because a cutoff never exceeds its present. Overflow (inputs
  // with no interleaving present — a stall or idle burst) drops the oldest,
  // advancing both cursors as needed, and is counted.
  static constexpr uint32_t kPending = 256;
  std::array<uint64_t, kPending> pending_{};
  uint32_t head_{0};        // frame-accurate drain (removes)
  uint32_t floor_head_{0};  // floor read-ahead (does not remove)
  uint32_t tail_{0};
  uint32_t dropped_{0};

  Hist window_{};         // frame-accurate, current window
  Hist session_{};        // frame-accurate, whole session
  Hist window_floor_{};   // floor, current window
  Hist session_floor_{};  // floor, whole session
  uint32_t presents_{0};

  [[nodiscard]] bool Empty() const { return head_ == tail_; }
};

}  // namespace profiling

#endif  // SHELL_PROFILING_MOTION_TO_PHOTON_H_
