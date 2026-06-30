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

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "configuration/configuration.h"

class IDisplay;
class Backend;

namespace backend {

// One compiled-in backend's runtime identity plus the two factory callables
// that used to live in the App::MakeDisplay / Backend::Create #if chains. The
// pair is cohesive: make_backend expects the concrete IDisplay that
// make_display produced for the same descriptor.
struct BackendDescriptor {
  std::string key;  // "wayland-egl","wayland-vulkan","drm-kms-egl",
                    // "drm-kms-vulkan","software"

  // Creates the IDisplay for this backend (the MakeDisplay branch body).
  std::function<std::shared_ptr<IDisplay>(
      const std::vector<Configuration::Config>&)>
      make_display;

  // Creates the Backend/backend (the flutter_view branch body), given the
  // display this backend's make_display produced. Returns null on init
  // failure (the caller fail-fasts).
  std::function<std::shared_ptr<Backend>(const Configuration::Config&,
                                         IDisplay*)>
      make_backend;

  // Optional: print the available DRM modes for --drm-list-modes. Set for the
  // DRM and software backends (which scan out to a DRM device); null for
  // backends with no DRM device (e.g. Wayland). @p device may be empty (the
  // hook applies a sensible default). Returns 0 on success.
  std::function<int(const std::string& device)> list_modes;
};

// Process-wide table of the compiled-in backends. Populated once at startup by
// RegisterCompiledBackends, the active descriptor is resolved once before App
// is constructed and read-only thereafter, so no locking is required.
class BackendRegistry {
 public:
  static BackendRegistry& Instance();  // Meyers singleton.

  void Register(BackendDescriptor d);

  // Returns the descriptor for @p key, or null if not registered.
  [[nodiscard]] const BackendDescriptor* Resolve(std::string_view key) const;

  [[nodiscard]] std::vector<std::string> Keys() const;

  // Resolved once at startup; read-only afterward.
  void SetActive(const BackendDescriptor* d);

  // Asserts a descriptor has been made active.
  [[nodiscard]] const BackendDescriptor& Active() const;

  // True once SetActive has been called. Lets a caller resolve the active
  // backend lazily (and without tripping Active()'s assert).
  [[nodiscard]] bool HasActive() const { return active_ != nullptr; }

 private:
  std::vector<BackendDescriptor> entries_;
  const BackendDescriptor* active_ = nullptr;
};

}  // namespace backend
