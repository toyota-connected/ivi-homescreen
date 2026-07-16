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

#ifndef SHELL_PROFILING_PV_LATENCY_H_
#define SHELL_PROFILING_PV_LATENCY_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <unordered_set>

// Opt-in probe (IVI_PV_LATENCY=1) that measures the delay a platform view adds
// after a pointer interaction: the wall-clock from the last pointer down/up to
// the first frame a newly created platform view is composited into. Splits the
// perceived "tap -> native box appears" lag into the framework-internal segment
// (input -> the embedder is asked to create the view) and the embedder segment
// (create -> first composite), which is logged separately at registration.
namespace ivi::pv_latency {

inline bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("IVI_PV_LATENCY");
    return e != nullptr && e[0] == '1';
  }();
  return on;
}

inline uint64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Monotonic ns of the most recent pointer down/up (the tap boundary). Read at
// first composite to bound the input -> box-visible latency.
inline std::atomic<uint64_t>& LastInputNs() {
  static std::atomic<uint64_t> v{0};
  return v;
}

inline void MarkInput() {
  if (Enabled()) {
    LastInputNs().store(NowNs(), std::memory_order_relaxed);
  }
}

// The first time platform view @p id is composited, return the input ->
// box-visible latency in microseconds; -1 on every later call for that id (so a
// caller logs each new view once), when disabled, or when no pointer input
// preceded it (startup views). Ids are never reused, so the seen set only
// grows.
inline int64_t FirstCompositeLatencyUs(int64_t id) {
  if (!Enabled()) {
    return -1;
  }
  static std::mutex m;
  static std::unordered_set<int64_t> seen;
  std::lock_guard<std::mutex> lk(m);
  if (!seen.insert(id).second) {
    return -1;  // already reported this view
  }
  const uint64_t t0 = LastInputNs().load(std::memory_order_relaxed);
  const uint64_t now = NowNs();
  if (t0 == 0 || now <= t0) {
    return -1;  // no triggering input (e.g. a view created at startup)
  }
  return static_cast<int64_t>((now - t0) / 1000);
}

}  // namespace ivi::pv_latency

#endif  // SHELL_PROFILING_PV_LATENCY_H_
