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

#include "startup_plan.h"

namespace ihs::osgi {

std::chrono::milliseconds StartupPlan::CriticalBudget() const {
  std::chrono::milliseconds total{0};
  for (const auto& bundle : critical) {
    total += bundle.startup_timeout;
  }
  return total;
}

std::chrono::milliseconds StartupPlan::NormalSpan() const {
  if (normal.empty()) {
    return std::chrono::milliseconds{0};
  }
  // The launch offset of the last bundle: the span is until the final launch,
  // not until it finishes -- nothing waits on a normal bundle.
  return normal.back().start_delay;
}

std::vector<std::string> StartupPlan::LaunchOrder() const {
  std::vector<std::string> names;
  names.reserve(size());
  for (const auto* phase : {&critical, &normal, &background}) {
    for (const auto& bundle : *phase) {
      names.push_back(bundle.symbolic_name);
    }
  }
  return names;
}

StartupPlan BuildStartupPlan(const std::vector<BundleManifest>& manifests,
                             const StartupPolicy& policy) {
  StartupPlan plan;

  for (size_t index = 0; index < manifests.size(); ++index) {
    const BundleManifest& manifest = manifests[index];

    PlannedBundle planned;
    planned.symbolic_name = manifest.symbolic_name;
    planned.priority = manifest.priority;
    planned.manifest_index = index;
    planned.cpu_core = manifest.cpu_core;

    switch (manifest.priority) {
      case Priority::kCritical:
        // No delay and a real deadline: these are launched back to back and
        // each one is awaited before the next.
        planned.startup_timeout =
            std::chrono::milliseconds{manifest.startup_timeout_ms};
        plan.critical.push_back(std::move(planned));
        break;

      case Priority::kNormal:
        // Staggered by position within the phase, so the Nth bundle waits N
        // gaps rather than every bundle launching at once.
        planned.start_delay = policy.normal_stagger * plan.normal.size();
        plan.normal.push_back(std::move(planned));
        break;

      case Priority::kBackground:
        // Detached and never awaited, so neither a delay nor a deadline means
        // anything here.
        plan.background.push_back(std::move(planned));
        break;
    }
  }

  return plan;
}

}  // namespace ihs::osgi
