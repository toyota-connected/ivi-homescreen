/*
 * Copyright 2020-2026 Toyota Connected North America
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

#include "display/output_identity.h"

#include <memory>
#include <set>

#include <libudev.h>

#include "logging/logging.h"

namespace homescreen {
namespace {

// The property an integrator's udev rule sets on a DRM connector.
constexpr const char* kOutputNameProperty = "IHS_OUTPUT_NAME";

struct UdevDeleter {
  void operator()(udev* u) const noexcept { udev_unref(u); }
};
struct EnumerateDeleter {
  void operator()(udev_enumerate* e) const noexcept { udev_enumerate_unref(e); }
};
struct DeviceDeleter {
  void operator()(udev_device* d) const noexcept { udev_device_unref(d); }
};

// "card1-DSI-1" -> "DSI-1", which is the form OutputInfo::name carries. Returns
// empty for a sysname that is not <card>-<connector>.
std::string_view ConnectorPart(const std::string_view sysname) {
  const auto dash = sysname.find('-');
  if (dash == std::string_view::npos || dash + 1 >= sysname.size()) {
    return {};
  }
  return sysname.substr(dash + 1);
}

// "card1-DSI-1" -> "card1".
std::string_view CardPart(const std::string_view sysname) {
  const auto dash = sysname.find('-');
  return dash == std::string_view::npos ? sysname : sysname.substr(0, dash);
}

}  // namespace

OutputIdMap ReadOutputIds(const std::string_view card_sysname) {
  OutputIdMap ids;

  const std::unique_ptr<udev, UdevDeleter> ctx{udev_new()};
  if (!ctx) {
    ihs::log::warn("[OutputIdentity] udev_new failed; no role names available");
    return ids;
  }

  const std::unique_ptr<udev_enumerate, EnumerateDeleter> en{
      udev_enumerate_new(ctx.get())};
  if (!en) {
    return ids;
  }
  // Connectors only: a card, a render node and a connector all live in the drm
  // subsystem, and only the last carries this property.
  udev_enumerate_add_match_subsystem(en.get(), "drm");
  udev_enumerate_add_match_property(en.get(), "DEVTYPE", "drm_connector");
  if (udev_enumerate_scan_devices(en.get()) < 0) {
    return ids;
  }

  // Connectors seen on more than one card with conflicting roles; see below.
  std::set<std::string, std::less<>> ambiguous;

  udev_list_entry* entry = nullptr;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en.get())) {
    const char* syspath = udev_list_entry_get_name(entry);
    if (syspath == nullptr) {
      continue;
    }
    const std::unique_ptr<udev_device, DeviceDeleter> dev{
        udev_device_new_from_syspath(ctx.get(), syspath)};
    if (!dev) {
      continue;
    }
    const char* sysname = udev_device_get_sysname(dev.get());
    if (sysname == nullptr) {
      continue;
    }
    // An empty card_sysname means the caller could not identify its card (it
    // was handed an fd rather than opening a node); take every connector then,
    // which is right for the single-card case and the best available guess
    // otherwise.
    if (!card_sysname.empty() && CardPart(sysname) != card_sysname) {
      continue;
    }
    const char* value =
        udev_device_get_property_value(dev.get(), kOutputNameProperty);
    if (value == nullptr || *value == '\0') {
      continue;  // no rule for this connector; the common case
    }
    const std::string_view connector = ConnectorPart(sysname);
    if (connector.empty() || ambiguous.find(connector) != ambiguous.end()) {
      continue;
    }
    const auto [it, inserted] =
        ids.emplace(std::string(connector), std::string(value));
    if (!inserted && it->second != value) {
      // Only reachable on the any-card path: within one card a connector
      // sysname occurs once. Two cards naming the same connector to different
      // roles leaves nothing here to say which card the caller is on, so
      // neither answer is usable. Drop the entry rather than keep whichever
      // udev enumerated first -- the role then matches no output and the view
      // parks with a warning, which is visible, instead of binding to an
      // arbitrary card, which is not.
      ihs::log::error(
          "[OutputIdentity] connector '{}' is mapped to both '{}' and '{}' on "
          "different cards; ignoring {} for it",
          connector, it->second, value, kOutputNameProperty);
      ids.erase(it);
      ambiguous.emplace(connector);
    }
  }

  if (!ids.empty()) {
    ihs::log::debug("[OutputIdentity] {} connector(s) carry {}", ids.size(),
                    kOutputNameProperty);
  }
  return ids;
}

}  // namespace homescreen
