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

#include "display/drm_device_resolver.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

#include <drm_mode.h>  // DRM_MODE_CONNECTOR_VIRTUAL

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/display/mode_list.hpp>

#include "logging/logging.h"

namespace homescreen {
namespace {

// A probed card node and what it exposes, retained for the failure diagnostic.
struct CardProbe {
  std::string path;
  bool has_connected_real = false;  // a connected, non-virtual connector
  std::string summary;              // "DP-1(connected) HDMI-A-1(disconnected)"
};

CardProbe ProbeCard(const std::string& path) {
  CardProbe probe;
  probe.path = path;

  auto dev = drm::Device::open(path);
  if (!dev) {
    probe.summary = "open failed: " + dev.error().message();
    return probe;
  }
  auto connectors = drm::display::query_connector_modes(*dev);
  if (!connectors) {
    probe.summary = "query failed: " + connectors.error().message();
    return probe;
  }

  std::string summary;
  for (const auto& connector : *connectors) {
    const bool is_virtual =
        connector.connector_type == DRM_MODE_CONNECTOR_VIRTUAL;
    if (connector.connected && !is_virtual) {
      probe.has_connected_real = true;
    }
    if (!summary.empty()) {
      summary += ' ';
    }
    summary += connector.name();
    summary += connector.connected ? "(connected)" : "(disconnected)";
  }
  probe.summary = summary.empty() ? "no connectors" : summary;
  return probe;
}

// The /dev/dri/cardN primary nodes that exist, sorted. Excludes renderD*/
// controlD* (no KMS) -- only "card" + digits.
std::vector<std::string> ListCardNodes() {
  std::vector<std::string> cards;
  std::error_code ec;
  const std::filesystem::path dri{"/dev/dri"};
  for (std::filesystem::directory_iterator it{dri, ec}, end; !ec && it != end;
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() > 4 && name.rfind("card", 0) == 0 &&
        name.find_first_not_of("0123456789", 4) == std::string::npos) {
      cards.push_back(it->path().string());
    }
  }
  std::sort(cards.begin(), cards.end());
  return cards;
}

}  // namespace

std::optional<std::string> ResolveDrmDevice(
    const std::optional<std::string>& explicit_device) {
  // 1. Explicit selection wins, verbatim -- no probing.
  if (explicit_device.has_value() && !explicit_device->empty()) {
    return *explicit_device;
  }

  // 2. Look at the card nodes that actually exist (never assume card0/card1).
  const std::vector<std::string> cards = ListCardNodes();
  if (cards.empty()) {
    ihs::log::critical("[drm] no /dev/dri/card* node present");
    return std::nullopt;
  }

  // 3. A single card is the only choice -- single-GPU systems (whatever the
  //    number) and headless vkms/CI.
  if (cards.size() == 1) {
    ihs::log::info("[drm] auto-selected the only DRM card present: {}",
                   cards.front());
    return cards.front();
  }

  // 4. Several cards: the first with a connected, non-virtual display, so a
  //    headless vkms/Virtual node never shadows the real GPU.
  std::vector<CardProbe> probes;
  probes.reserve(cards.size());
  for (const auto& card : cards) {
    probes.push_back(ProbeCard(card));
  }
  for (const auto& probe : probes) {
    if (probe.has_connected_real) {
      ihs::log::info(
          "[drm] auto-selected first card with a connected display: {} ({}) -- "
          "set [view.backend.drm] device / --drm-device to override",
          probe.path, probe.summary);
      return probe.path;
    }
  }

  // 5. Ambiguous: several cards, none with a connected display. Refuse and
  //    print the map rather than guess into a black screen.
  ihs::log::critical(
      "[drm] {} DRM cards present, none with a connected display; select one "
      "explicitly via --drm-device / [view.backend.drm] device:",
      cards.size());
  for (const auto& probe : probes) {
    ihs::log::critical("[drm]   {}: {}", probe.path, probe.summary);
  }
  return std::nullopt;
}

}  // namespace homescreen
