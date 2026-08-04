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

#include "display/connector_edid.h"
#include "display/output_identity.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/display/mode_list.hpp>

#include "logging/logging.h"

namespace homescreen {

DrmOutputProvider::DrmOutputProvider(std::string device_path)
    : device_path_(std::move(device_path)) {}

void DrmOutputProvider::SetEnumerationFd(int fd) {
  enumeration_fd_ = fd;
}

namespace {

// The sysfs name udev keys connectors by ("card1"), derived from the open fd
// rather than from the path it came from.
//
// Not the path's basename: the device may legitimately be named through a
// /dev/dri/by-path/ symlink -- which is what the documentation recommends,
// precisely because cardN is assigned in probe order -- and "platform-gpu-card"
// is not a sysfs name. Going through the device number resolves the symlink,
// and works equally for a leased fd that has no path at all.
//
// Empty when the number cannot be resolved; ReadOutputIds then matches
// connectors on any card, which is right for a single-card system.
std::string CardSysnameFromFd(const int fd) {
  struct stat st{};
  if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
    return {};
  }
  const std::string link = "/sys/dev/char/" +
                           std::to_string(major(st.st_rdev)) + ":" +
                           std::to_string(minor(st.st_rdev));
  std::error_code ec;
  const auto target = std::filesystem::read_symlink(link, ec);
  if (ec) {
    return {};
  }
  return target.filename().string();  // ".../drm/card1" -> "card1"
}

}  // namespace

std::vector<OutputInfo> DrmOutputProvider::EnumerateOutputs() const {
  std::vector<OutputInfo> outputs;

  // Prefer a supplied fd (a DRM lease) over opening the card: it is both the
  // correctly-scoped view (the kernel filters it to the leased objects) and the
  // only handle a leased client is guaranteed to have. from_fd borrows -- the
  // fd stays owned by whoever supplied it.
  std::optional<drm::Device> dev;
  if (enumeration_fd_ >= 0) {
    dev.emplace(drm::Device::from_fd(enumeration_fd_));
  } else {
    auto opened = drm::Device::open(device_path_);
    if (!opened) {
      ihs::log::warn("[DrmOutputProvider] open('{}'): {}", device_path_,
                     opened.error().message());
      return outputs;
    }
    dev.emplace(std::move(*opened));
  }

  auto connectors = drm::display::query_connector_modes(*dev);
  if (!connectors) {
    ihs::log::warn("[DrmOutputProvider] query_connector_modes('{}'): {}",
                   device_path_, connectors.error().message());
    return outputs;
  }

  // Role names, if an integrator's udev rule assigns any. Read once per
  // enumeration rather than per connector: one udev scan answers for the whole
  // card. Empty without a rule, which is the default and costs nothing.
  const OutputIdMap role_names = ReadOutputIds(CardSysnameFromFd(dev->fd()));

  outputs.reserve(connectors->size());
  for (const auto& connector : *connectors) {
    OutputInfo info;
    info.name = connector.name();
    info.connected = connector.connected;
    info.handle = connector.connector_id;
    if (const auto it = role_names.find(info.name); it != role_names.end()) {
      info.output_id = it->second;
    }

    // Monitor identity from the connector's EDID. Without it OutputInfo::serial
    // is always empty, so the edid_serial tier of ResolveOutput can never fire
    // and a [view.output] serial= silently falls through to the connector name
    // -- a documented match that could not work. A port with nothing attached,
    // or a panel whose EDID does not parse, simply leaves these empty.
    //
    // Note the serial is only as unique as the panel maker chose to make it:
    // two LG DQHD heads measured here report the same serial, the same model
    // string and the same make, so a serial= match against them is ambiguous
    // and ResolveOutput says so before falling through to the connector name.
    if (const auto edid =
            ProbeConnectorInfo(dev->fd(), connector.connector_id)) {
      info.make = edid->make;
      info.model = edid->model;
      info.serial = edid->serial;
      info.mm_width = edid->width_mm;
      info.mm_height = edid->height_mm;
    } else if (connector.connected) {
      // Routine, not a fault: a panel wired to a fixed port -- DSI on an
      // embedded board is the usual case -- often carries no EDID at all, and
      // plenty that do carry one omit the serial. Such an output can only be
      // identified by its connector name, so say so rather than leave someone
      // wondering why a serial= match never binds.
      ihs::log::debug(
          "[DrmOutputProvider] '{}' reports no usable EDID; identity is the "
          "connector name alone",
          info.name);
    }

    // Current extent + refresh come from the connector's preferred mode (the
    // EDID-preferred entry, else the first advertised).
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
