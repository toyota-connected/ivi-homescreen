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

#include "display/drm_output_provider.h"

#include <utility>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/display/mode_list.hpp>

#include "logging/logging.h"

namespace homescreen {

DrmOutputProvider::DrmOutputProvider(std::string device_path)
    : device_path_(std::move(device_path)) {}

std::vector<OutputInfo> DrmOutputProvider::EnumerateOutputs() const {
  std::vector<OutputInfo> outputs;

  auto dev = drm::Device::open(device_path_);
  if (!dev) {
    spdlog::warn("[DrmOutputProvider] open('{}'): {}", device_path_,
                 dev.error().message());
    return outputs;
  }

  auto connectors = drm::display::query_connector_modes(*dev);
  if (!connectors) {
    spdlog::warn("[DrmOutputProvider] query_connector_modes('{}'): {}",
                 device_path_, connectors.error().message());
    return outputs;
  }

  outputs.reserve(connectors->size());
  for (const auto& connector : *connectors) {
    OutputInfo info;
    info.name = connector.name();
    info.connected = connector.connected;
    info.handle = connector.connector_id;

    // Current extent + refresh come from the connector's preferred mode (the
    // EDID-preferred entry, else the first advertised). EDID make/model/serial
    // are not read here yet; name is the join key ResolveOutput uses.
    const drm::ModeInfo* mode = nullptr;
    for (const auto& candidate : connector.modes) {
      if (candidate.preferred()) {
        mode = &candidate;
        break;
      }
    }
    if (mode == nullptr && !connector.modes.empty()) {
      mode = &connector.modes.front();
    }
    if (mode != nullptr) {
      info.width_px = static_cast<int32_t>(mode->width());
      info.height_px = static_cast<int32_t>(mode->height());
      info.refresh_hz = static_cast<double>(mode->refresh());
    }

    outputs.push_back(std::move(info));
  }
  return outputs;
}

void DrmOutputProvider::SetOutputListener(IOutputListener* listener) {
  listener_ = listener;
}

bool DrmOutputProvider::SupportsHotplug() const {
  // DRM connectors hotplug, but the udev monitor that would diff the connector
  // set and drive the listener is not wired here yet -- so outputs are
  // enumerated on demand and no change callbacks are delivered. This reports
  // that honestly (per the method contract) until the monitor lands.
  return false;
}

}  // namespace homescreen
