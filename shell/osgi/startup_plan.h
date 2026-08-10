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

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "bundle_manifest.h"

namespace ihs::osgi {

// One bundle's place in the startup sequence.
struct PlannedBundle {
  std::string symbolic_name;
  Priority priority{Priority::kNormal};

  // Index into the manifest vector the plan was built from, so the orchestrator
  // can reach the full Configuration::Config without the plan copying it.
  size_t manifest_index{0};

  // CPU to pin this bundle's engine thread to; -1 leaves it unpinned. Already
  // validated against the process affinity mask at parse time.
  int cpu_core{-1};

  // Delay from the start of this bundle's phase before it is launched. Zero for
  // critical (they are launched back to back and waited on) and background.
  std::chrono::milliseconds start_delay{0};

  // How long to wait for this bundle to report ACTIVE. Only meaningful for
  // critical bundles, which are the only ones blocked on.
  std::chrono::milliseconds startup_timeout{0};
};

// Knobs for BuildStartupPlan.
struct StartupPolicy {
  // Gap between consecutive normal-priority bundles. They are staggered so
  // several engines do not contend for the same first-frame resources -- disk
  // for the AOT snapshot, and the GPU for the first pipeline compile.
  std::chrono::milliseconds normal_stagger{50};
};

// The startup sequence, split by the point at which each phase runs relative to
// the asio reactor.
struct StartupPlan {
  // Launched and waited on before the reactor runs.
  std::vector<PlannedBundle> critical;
  // Launched after the reactor is running, staggered.
  std::vector<PlannedBundle> normal;
  // Detached; never waited on.
  std::vector<PlannedBundle> background;

  // Worst-case time the reactor is blocked before it can run: the sum of the
  // critical bundles' timeouts, since they are awaited one after another.
  //
  // Surfaced as a number rather than left implicit because it is the figure
  // that decides whether a config is viable at all -- four critical bundles at
  // the 500 ms default is two seconds of black screen before anything is
  // serviced, which is a decision an integrator should make deliberately.
  [[nodiscard]] std::chrono::milliseconds CriticalBudget() const;

  // Time from the reactor starting until the last normal bundle is launched.
  [[nodiscard]] std::chrono::milliseconds NormalSpan() const;

  [[nodiscard]] size_t size() const {
    return critical.size() + normal.size() + background.size();
  }

  [[nodiscard]] bool empty() const { return size() == 0; }

  // Every bundle in launch order: critical, then normal, then background.
  [[nodiscard]] std::vector<std::string> LaunchOrder() const;
};

// Sort @manifests into startup phases.
//
// Within a phase, declaration order from the config is preserved. That is the
// only ordering an integrator can actually control -- there is no secondary key
// to sort on that would mean anything -- so the config file reads as the
// startup script it is.
StartupPlan BuildStartupPlan(const std::vector<BundleManifest>& manifests,
                             const StartupPolicy& policy = {});

}  // namespace ihs::osgi
