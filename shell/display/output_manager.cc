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

#include "display/output_manager.h"

#include <vector>

#include "display/idisplay.h"
#include "display/output_provider.h"
#include "logging/logging.h"

namespace homescreen {

std::optional<std::string> OutputManager::ResolveForView(
    const Configuration::Config& config,
    IDisplay* display,
    BackendFamily family) {
  const OutputMatch& match = config.view.output;
  if (match.empty()) {
    // No [view.output] constraint: leave the backend's own default / auto-pick
    // in place (preserves single-view behavior).
    return std::nullopt;
  }

  IOutputProvider* provider =
      display != nullptr ? display->GetOutputProvider() : nullptr;
  if (provider == nullptr) {
    ihs::log::warn(
        "[OutputManager] a view requests a specific output but its display "
        "exposes no output provider; ignoring the request");
    return std::nullopt;
  }

  const std::vector<OutputInfo> outputs = provider->EnumerateOutputs();
  std::optional<std::string> name = ResolveOutput(match, outputs, family);
  if (!name.has_value()) {
    // The requested output is not among the connected ones. The caller keeps
    // its default; full parking-until-present lands with the hotplug listener.
    ihs::log::warn(
        "[OutputManager] requested output is not connected (wl_name='{}' "
        "drm_connector='{}' serial='{}'); falling back to the default output",
        match.wl_name.value_or("-"), match.drm_connector.value_or("-"),
        match.edid_serial.value_or("-"));
    return std::nullopt;
  }

  ihs::log::info("[OutputManager] view bound to output '{}'", *name);
  return name;
}

}  // namespace homescreen
