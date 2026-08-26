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

#include <map>
#include <mutex>
#include <string>

#include "bundle_host.h"
#include "vsync_coordinator.h"

class App;
class FlutterView;

namespace ihs::osgi {

// The production IBundleHost: brings a bundle up as a real Flutter view on a
// real backend.
//
// Spawn is App::AddView, which is the piece that makes "critical first" mean
// anything. App's constructor builds every view back to back and
// FlutterView::Initialize() runs the engine, so a bundle expressed as just
// another config in that vector would already be competing for the GPU before
// the orchestrator could wait on anything -- the ordering would be observed,
// not enforced. Adding views one at a time is what lets the orchestrator hold
// the reactor until the cluster is actually up.
//
// Threading: the orchestrator calls Spawn/PinThread/Shutdown from the startup
// thread, before the reactor runs for critical bundles and from the deferred
// phase afterwards. App is not thread-safe, so all three are serialized on
// mutex_ and are expected to be called from one thread at a time.
class AppBundleHost final : public IBundleHost {
 public:
  // @coordinator may be null when the shell was built without a vsync source
  // to fan out; bundles then drive their own vsync as an unmanaged view does.
  AppBundleHost(App& app, VsyncCoordinator* coordinator);

  BundleHandle Spawn(const BundleManifest& manifest) override;
  bool PinThread(BundleHandle handle, int cpu_core) override;
  void Shutdown(BundleHandle handle) override;

 private:
  struct Live {
    FlutterView* view{nullptr};
    std::string symbolic_name;
  };

  App& app_;
  VsyncCoordinator* coordinator_;

  mutable std::mutex mutex_;
  BundleHandle next_handle_{0};
  std::map<BundleHandle, Live> live_;
};

}  // namespace ihs::osgi
