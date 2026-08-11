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

#include "app_bundle_host.h"

#include <pthread.h>
#include <sched.h>

#include <utility>

#include "app.h"
#include "logging/logging.h"
#include "task_runner.h"
#include "view/flutter_view.h"

namespace ihs::osgi {

AppBundleHost::AppBundleHost(App& app, VsyncCoordinator* coordinator)
    : app_(app), coordinator_(coordinator) {}

BundleHandle AppBundleHost::Spawn(const BundleManifest& manifest) {
  std::lock_guard lock(mutex_);

  // One view, right now. Everything the bundle's [osgi.bundles.*] table said --
  // backend, DRM connector, args, output pinning -- is already in this config,
  // because it was built by the same parser a [[view]] goes through.
  FlutterView* view = app_.AddView(manifest.config);
  if (view == nullptr) {
    ihs::log::error("[osgi] bundle '{}': could not create a view",
                    manifest.symbolic_name);
    return kInvalidBundleHandle;
  }

  const BundleHandle handle = ++next_handle_;
  live_.emplace(handle, Live{view, manifest.symbolic_name});
  return handle;
}

bool AppBundleHost::PinThread(const BundleHandle handle, const int cpu_core) {
  std::lock_guard lock(mutex_);
  const auto it = live_.find(handle);
  if (it == live_.end()) {
    return false;
  }

  TaskRunner* runner = it->second.view->GetPlatformTaskRunner();
  if (runner == nullptr) {
    ihs::log::error("[osgi] bundle '{}': no platform task runner to pin",
                    it->second.symbolic_name);
    return false;
  }

  // The engine's platform thread is the one that matters: it runs the Dart
  // isolate's platform-side work and is where a missed deadline shows up as
  // jitter. cpu_core was already validated against the process affinity mask at
  // parse time, so a refusal here is the kernel declining, not a bad config.
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<size_t>(cpu_core), &set);
  if (const int rc =
          pthread_setaffinity_np(runner->NativeHandle(), sizeof(set), &set);
      rc != 0) {
    ihs::log::error(
        "[osgi] bundle '{}': pthread_setaffinity_np to CPU {} failed ({})",
        it->second.symbolic_name, cpu_core, rc);
    return false;
  }
  ihs::log::debug("[osgi] bundle '{}': engine thread pinned to CPU {}",
                  it->second.symbolic_name, cpu_core);
  return true;
}

void AppBundleHost::Shutdown(const BundleHandle handle) {
  std::lock_guard lock(mutex_);
  const auto it = live_.find(handle);
  if (it == live_.end()) {
    return;
  }

  // Deregister from the vsync fan-out before the view goes: a coordinator
  // holding a target into a destroyed view would deliver a tick into freed
  // memory on the next vblank.
  if (coordinator_ != nullptr) {
    coordinator_->Remove(it->second.symbolic_name);
  }
  app_.RemoveView(it->second.view);
  ihs::log::debug("[osgi] bundle '{}': view torn down",
                  it->second.symbolic_name);
  live_.erase(it);
}

}  // namespace ihs::osgi
