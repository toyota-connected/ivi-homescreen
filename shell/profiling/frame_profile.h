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

#pragma once

#include <cstdint>
#include <string_view>

namespace profiling {

// Unified frame-pacing profiler shared by every backend. A backend calls
// Record() once per present and LogSessionSummary() from its destructor. When
// enabled it emits, with the backend's label:
//
//   * a window line every kWindow presents:
//       [<label>] profile (n=N): fps=.. mean_interval=..us max_interval=..us
//       discarded=.. buckets[60Hz/30Hz/20Hz/slow/idle]=../../../../..
//   * a session summary on teardown (folds the trailing partial window):
//       [<label>] session summary: frames=N fps=.. mean_interval=..us
//       max_interval=..us discarded=..
//       [<label>] session buckets: 60Hz=.. (..%) 30Hz=.. (..%) ...
//
// The interval histogram uses fixed thresholds (<=17 / <=33 / <=50 / <=100 ms,
// then idle) so the numbers line up across backends regardless of how each
// one drives presents.
//
// Enabled by the IVI_PROFILE environment variable. Each backend may also pass
// its historical per-backend gate (e.g. "IVI_SW_PROFILE") so existing scripts
// keep working. Evaluate Enabled() once per process and cache it at the call
// site — Record() is on the per-frame path and does not re-check the env.
class FrameProfile {
 public:
  // True when IVI_PROFILE is set, or (when non-null) the legacy gate is set.
  static bool Enabled(const char* legacy_env = nullptr);

  // Record one present. When ok is false the frame is counted as discarded
  // (superseded / not scanned out) and contributes no interval. now_ns is a
  // CLOCK_MONOTONIC timestamp; pass 0 to sample it internally. Set stalled when
  // the present had to block waiting for a buffer/flip — under correct pacing
  // and buffer depth this stays zero, so a nonzero count flags a stall. label
  // is the backend tag for the window log line.
  void Record(std::string_view label,
              bool ok,
              uint64_t now_ns = 0,
              bool stalled = false);

  // Emit the session-aggregate summary. No-op when no frames were recorded.
  void LogSessionSummary(std::string_view label) const;

 private:
  static constexpr uint32_t kWindow = 60;

  struct Stats {
    uint64_t interval_sum_ns{0};
    uint64_t interval_max_ns{0};
    uint32_t frames{0};
    uint32_t discarded{0};
    uint32_t stalls{0};  // presents that blocked on a buffer/flip
    uint32_t b60{0};     // <= 17ms  (60Hz)
    uint32_t b30{0};     // 18-33ms  (30Hz)
    uint32_t b20{0};     // 34-50ms  (20Hz)
    uint32_t bslow{0};   // 51-100ms
    uint32_t bidle{0};   // > 100ms

    void AddInterval(uint64_t dt_ns);
    void Merge(const Stats& other);
  };

  uint64_t last_present_ns_{0};
  Stats window_{};
  Stats session_{};
};

}  // namespace profiling