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
#include <string>

#include "configuration/configuration.h"
#include "priority.h"

namespace ihs::osgi {

// One [[osgi.bundles]] entry.
//
// `config` is a full Configuration::Config built by the same code path that
// builds a [[view]] entry, so a bundle may use any [view.*] key -- backend,
// output pinning, args, shell/window type -- with identical semantics. The
// OSGi-specific keys are the ones alongside it.
struct BundleManifest {
  // OSGi symbolic name; unique within a process. Required.
  std::string symbolic_name;

  // Bring-up tier. Defaults to kNormal.
  Priority priority{Priority::kNormal};

  // CPU to pin this bundle's engine thread to via pthread_setaffinity_np.
  // -1 (the default) leaves the thread unpinned. Validated at parse time
  // against the process affinity mask, not against a core count -- see
  // cpu_affinity.h for why those differ.
  int cpu_core{-1};

  // How long StartCriticalBundles() waits for this bundle to report ACTIVE.
  // Ignored for kNormal / kBackground, which are never waited on.
  int startup_timeout_ms{500};

  // Everything a [[view]] entry can express, including view.bundle_path.
  Configuration::Config config;
};

}  // namespace ihs::osgi
