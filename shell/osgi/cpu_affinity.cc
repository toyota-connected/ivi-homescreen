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

#include "cpu_affinity.h"

#include <sched.h>

#include "logging/logging.h"

namespace ihs::osgi {

namespace {

// The mask is read once. A process's affinity can change under it
// (sched_setaffinity from outside), but re-reading per query would let one
// bundle validate against a different mask than the next, turning config
// validation into a race. One snapshot, taken before any bundle starts, keeps
// the whole config judged against a single view of the machine.
struct AffinitySnapshot {
  cpu_set_t set{};
  bool known{false};

  AffinitySnapshot() {
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
      known = true;
      return;
    }
    // Not fatal: affinity only tunes scheduling. Fall through with known=false
    // so validation admits any core rather than refusing to start.
    ihs::log::warn(
        "[osgi] sched_getaffinity failed; cpu_core values will not be "
        "validated against the available CPUs");
  }
};

const AffinitySnapshot& Snapshot() {
  static const AffinitySnapshot snapshot;
  return snapshot;
}

}  // namespace

bool CpuAffinityKnown() {
  return Snapshot().known;
}

bool IsCpuAvailable(const int cpu) {
  // CPU_ISSET is undefined outside [0, CPU_SETSIZE), so bound before asking.
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  const auto& snapshot = Snapshot();
  if (!snapshot.known) {
    return true;
  }
  return CPU_ISSET(static_cast<size_t>(cpu), &snapshot.set) != 0;
}

std::optional<int> HighestAvailableCpu() {
  const auto& snapshot = Snapshot();
  if (!snapshot.known) {
    return std::nullopt;
  }
  for (int cpu = CPU_SETSIZE - 1; cpu >= 0; --cpu) {
    if (CPU_ISSET(static_cast<size_t>(cpu), &snapshot.set) != 0) {
      return cpu;
    }
  }
  return std::nullopt;
}

std::string AvailableCpuList() {
  const auto& snapshot = Snapshot();
  if (!snapshot.known) {
    return {};
  }

  std::string out;
  int run_start = -1;
  // <= CPU_SETSIZE so the final iteration closes a run ending at the last CPU.
  for (int cpu = 0; cpu <= CPU_SETSIZE; ++cpu) {
    const bool present =
        cpu < CPU_SETSIZE && CPU_ISSET(static_cast<size_t>(cpu), &snapshot.set);
    if (present && run_start < 0) {
      run_start = cpu;
    } else if (!present && run_start >= 0) {
      if (!out.empty()) {
        out += ',';
      }
      out += std::to_string(run_start);
      if (cpu - 1 != run_start) {
        out += '-';
        out += std::to_string(cpu - 1);
      }
      run_start = -1;
    }
  }
  return out;
}

}  // namespace ihs::osgi
