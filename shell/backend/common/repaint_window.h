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
#include <cstdlib>

// The repaint window: a present composites and commits at a fixed margin
// before the vblank it is meant for, rather than as soon as Flutter hands it
// over. A frame Flutter presents early in a refresh still reaches the display
// on the same vblank, and a producer buffer that lands in between -- a map
// frame rendered just after the flip -- is composited into it rather than
// waiting a whole refresh for the next one.
//
// How far before the vblank a present composites and commits by default: room
// for the composite and the commit, the rest of the refresh left for a
// producer's buffer to land in.
inline constexpr uint64_t kDefaultRepaintWindowNs = 5'000'000;

// How long to wait before compositing, given the time now, the last flip's
// vblank timestamp, the refresh period and the window (all nanoseconds,
// CLOCK_MONOTONIC). Zero means go now: the window is off, no flip has been
// seen, the window is already open, or the last flip is too old to count
// vblanks from (the period is rounded, so its phase drifts).
[[nodiscard]] constexpr uint64_t RepaintDelayNs(const uint64_t now_ns,
                                                const uint64_t last_flip_ns,
                                                const uint64_t period_ns,
                                                const uint64_t window_ns) {
  constexpr uint64_t kMaxPeriodsExtrapolated = 8;
  if (window_ns == 0 || period_ns == 0 || last_flip_ns == 0 ||
      now_ns < last_flip_ns || window_ns >= period_ns) {
    return 0;
  }
  const uint64_t since = now_ns - last_flip_ns;
  if (since >= kMaxPeriodsExtrapolated * period_ns) {
    return 0;
  }
  const uint64_t next_vblank =
      last_flip_ns + (since / period_ns + 1) * period_ns;
  const uint64_t opens = next_vblank - window_ns;
  return now_ns < opens ? opens - now_ns : 0;
}

// The window from IVI_DRM_REPAINT_WINDOW_US (microseconds before the vblank;
// 0 turns it off), else @default_ns.
[[nodiscard]] inline uint64_t RepaintWindowNsFromEnv(
    const uint64_t default_ns) {
  const char* env = std::getenv("IVI_DRM_REPAINT_WINDOW_US");
  if (env == nullptr || *env == '\0') {
    return default_ns;
  }
  char* end = nullptr;
  const unsigned long long us = std::strtoull(env, &end, 10);
  if (end == env || *end != '\0') {
    return default_ns;
  }
  return static_cast<uint64_t>(us) * 1000ULL;
}
