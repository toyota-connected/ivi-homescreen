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

#include <string_view>
#include <vector>

#include "display/output.h"

namespace homescreen {

// Receives output appear/disappear/change notifications on the platform (loop)
// thread, in batch — never mid-burst (the Wayland `done` event / the DRM
// debounced rescan is the barrier).
class IOutputListener {
 public:
  virtual ~IOutputListener() = default;

  virtual void OnOutputAdded(const OutputInfo& output) = 0;
  virtual void OnOutputRemoved(std::string_view name) = 0;
  virtual void OnOutputChanged(const OutputInfo& output) = 0;  // mode/EDID
};

// A backend's source of physical outputs. Each display owns one — the
// wl_registry for Wayland, the card fd for DRM — and exposes it through
// IDisplay::GetOutputProvider().
class IOutputProvider {
 public:
  virtual ~IOutputProvider() = default;

  IOutputProvider(const IOutputProvider&) = delete;
  IOutputProvider& operator=(const IOutputProvider&) = delete;

  // The currently known outputs: the connected ones, plus (on DRM) ports that
  // are enumerated but disconnected.
  [[nodiscard]] virtual std::vector<OutputInfo> EnumerateOutputs() const = 0;

  // Set the listener that receives add/remove/change callbacks; nullptr clears.
  virtual void SetOutputListener(IOutputListener* listener) = 0;

  // False for sources with no port model (e.g. the fbdev software sink): the
  // single output never changes, so no callbacks are delivered.
  [[nodiscard]] virtual bool SupportsHotplug() const = 0;

 protected:
  IOutputProvider() = default;
};

}  // namespace homescreen
