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

#include "profiling/frame_profile.h"

#include <cstdlib>
#include <ctime>

#include "spdlog/spdlog.h"

namespace profiling {

namespace {

uint64_t MonotonicNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Mean inter-present interval. The first frame of a run has no predecessor, so
// the divisor is one less than the frame count once any interval exists.
uint64_t MeanIntervalNs(uint64_t interval_sum_ns, uint32_t frames) {
  const uint32_t samples = interval_sum_ns > 0 ? frames - 1 : frames;
  return samples > 0 ? interval_sum_ns / samples : 0;
}

double FpsFromMean(uint64_t mean_ns) {
  return mean_ns > 0 ? 1e9 / static_cast<double>(mean_ns) : 0.0;
}

}  // namespace

void FrameProfile::Stats::AddInterval(const uint64_t dt_ns) {
  interval_sum_ns += dt_ns;
  if (dt_ns > interval_max_ns) {
    interval_max_ns = dt_ns;
  }
  if (dt_ns <= 17'000'000ULL) {
    ++b60;
  } else if (dt_ns <= 33'000'000ULL) {
    ++b30;
  } else if (dt_ns <= 50'000'000ULL) {
    ++b20;
  } else if (dt_ns <= 100'000'000ULL) {
    ++bslow;
  } else {
    ++bidle;
  }
}

void FrameProfile::Stats::Merge(const Stats& other) {
  interval_sum_ns += other.interval_sum_ns;
  if (other.interval_max_ns > interval_max_ns) {
    interval_max_ns = other.interval_max_ns;
  }
  frames += other.frames;
  failures += other.failures;
  b60 += other.b60;
  b30 += other.b30;
  b20 += other.b20;
  bslow += other.bslow;
  bidle += other.bidle;
}

bool FrameProfile::Enabled(const char* legacy_env) {
  if (std::getenv("IVI_PROFILE") != nullptr) {
    return true;
  }
  return legacy_env != nullptr && std::getenv(legacy_env) != nullptr;
}

void FrameProfile::Record(const std::string_view label,
                          const bool ok,
                          uint64_t now_ns) {
  if (!ok) {
    ++window_.failures;
    return;
  }
  if (now_ns == 0) {
    now_ns = MonotonicNs();
  }
  if (last_present_ns_ != 0) {
    window_.AddInterval(now_ns - last_present_ns_);
  }
  last_present_ns_ = now_ns;
  ++window_.frames;

  if (window_.frames < kWindow) {
    return;
  }
  const uint64_t mean_ns =
      MeanIntervalNs(window_.interval_sum_ns, window_.frames);
  spdlog::info(
      "[{}] profile (n={}): fps={:.2f} mean_interval={}us max_interval={}us "
      "present_failures={} buckets[60Hz/30Hz/20Hz/slow/idle]={}/{}/{}/{}/{}",
      label, window_.frames, FpsFromMean(mean_ns), mean_ns / 1000,
      window_.interval_max_ns / 1000, window_.failures, window_.b60,
      window_.b30, window_.b20, window_.bslow, window_.bidle);

  session_.Merge(window_);
  window_ = Stats{};  // reset the window; last_present_ns_ carries continuity
}

void FrameProfile::LogSessionSummary(const std::string_view label) const {
  // Fold the trailing partial window (frames since the last flush) into the
  // totals so short runs and the tail of long runs are not dropped.
  Stats s = session_;
  s.Merge(window_);
  if (s.frames == 0) {
    return;
  }
  const uint64_t mean_ns = MeanIntervalNs(s.interval_sum_ns, s.frames);
  spdlog::info(
      "[{}] session summary: frames={} fps={:.2f} mean_interval={}us "
      "max_interval={}us present_failures={}",
      label, s.frames, FpsFromMean(mean_ns), mean_ns / 1000,
      s.interval_max_ns / 1000, s.failures);

  if (const uint32_t total = s.b60 + s.b30 + s.b20 + s.bslow + s.bidle;
      total > 0) {
    const double inv = 100.0 / static_cast<double>(total);
    spdlog::info(
        "[{}] session buckets: 60Hz={} ({:.1f}%) 30Hz={} ({:.1f}%) "
        "20Hz={} ({:.1f}%) slow={} ({:.1f}%) idle={} ({:.1f}%)",
        label, s.b60, s.b60 * inv, s.b30, s.b30 * inv, s.b20, s.b20 * inv,
        s.bslow, s.bslow * inv, s.bidle, s.bidle * inv);
  }
}

}  // namespace profiling