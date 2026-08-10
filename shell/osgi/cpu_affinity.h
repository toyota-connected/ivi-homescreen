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

#include <optional>
#include <string>

namespace ihs::osgi {

// Which CPUs this process may actually be pinned to, from sched_getaffinity.
//
// This is deliberately not std::thread::hardware_concurrency(), and not a core
// *count*. Under a cpuset -- a container, systemd CPUAffinity=, taskset, or the
// core partitioning an IVI integrator applies to separate a cluster from
// infotainment -- the two disagree in both directions. Confined to CPUs 2,3 on
// a 32-way host, hardware_concurrency() still answers 32, while a count-based
// check would admit indices 0 and 1, which are precisely the ones
// pthread_setaffinity_np will refuse.
//
// The affinity mask is the same set pthread_setaffinity_np validates against,
// so it is the only bound that makes a config error at parse time and a pinning
// failure at startup agree.

// True when the process affinity mask was readable. When false, every
// IsCpuAvailable() query returns true: a sched_getaffinity failure must not
// block startup over a knob that only tunes scheduling.
[[nodiscard]] bool CpuAffinityKnown();

// True when @cpu may be pinned to. Out-of-range indices are always false.
[[nodiscard]] bool IsCpuAvailable(int cpu);

// The allowed CPUs in compact range form ("2-3", "0-7,16-23"), for diagnostics.
// Empty when the mask is unknown.
[[nodiscard]] std::string AvailableCpuList();

// Highest allowed CPU index, or nullopt when the mask is unknown or empty.
[[nodiscard]] std::optional<int> HighestAvailableCpu();

}  // namespace ihs::osgi
