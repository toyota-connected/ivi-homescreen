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

#include "bundle_manifest.h"

namespace ihs::osgi {

// Identifies one spawned bundle engine. Zero means "no engine".
using BundleHandle = uint64_t;
inline constexpr BundleHandle kInvalidBundleHandle = 0;

// Everything the orchestrator needs that requires a live Flutter engine.
//
// This seam exists because none of it is reachable from a unit test.
// LibFlutterEngine's export table is a private static published only by
// Load(), which dlopens a real shared library -- there is no way to substitute
// a fake, and adding one would mean changing machinery every backend depends
// on. Putting the engine behind an interface the OSGi code owns keeps that
// machinery untouched and, more importantly, makes the orchestrator's actual
// subject matter testable: deadline expiry, spawn failure, refused affinity and
// restart backoff are all states a real engine will not reproduce on demand.
//
// The production implementation (Backend::Create + FlutterEngineInitialize /
// Run + Engine::AddView) lands with the App integration, which is verified on a
// vkms rig rather than by unit tests.
class IBundleHost {
 public:
  virtual ~IBundleHost() = default;

  // Bring up an engine for @manifest and start it running.
  //
  // Returns kInvalidBundleHandle on failure. Must not block until the bundle is
  // ACTIVE: reaching ACTIVE is reported asynchronously through the bridge, and
  // the orchestrator is what decides how long to wait for it.
  virtual BundleHandle Spawn(const BundleManifest& manifest) = 0;

  // Pin @handle's engine thread to @cpu_core (pthread_setaffinity_np).
  // Only called for a core that already passed validation against the process
  // affinity mask at config-parse time, so a failure here is a runtime refusal,
  // not a bad config.
  virtual bool PinThread(BundleHandle handle, int cpu_core) = 0;

  // Tear the engine down. Must tolerate a handle whose bundle never became
  // ACTIVE -- that is the timeout path.
  virtual void Shutdown(BundleHandle handle) = 0;
};

}  // namespace ihs::osgi
