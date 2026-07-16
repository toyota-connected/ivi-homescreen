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

#include <string>
#include <vector>

#include "display/output_provider.h"

namespace homescreen {

// IOutputProvider for a DRM card: enumerates the card's connectors as outputs.
// The card is a master domain exactly like a Wayland compositor connection --
// one device, N connectors -- so this is the DRM counterpart of the Wayland
// provider on Display. Enumeration uses libdrm only (no DRM master, no GL/GBM),
// so it is safe to call before, and independently of, the backend that scans
// out: the same connectors the kernel exposes are read directly here and
// re-exposed to the compositor connection's wl_outputs there.
class DrmOutputProvider final : public IOutputProvider {
 public:
  explicit DrmOutputProvider(std::string device_path);
  ~DrmOutputProvider() override = default;

  [[nodiscard]] std::vector<OutputInfo> EnumerateOutputs() const override;
  void SetOutputListener(IOutputListener* listener) override;
  [[nodiscard]] bool SupportsHotplug() const override;

  // Restrict enumeration to these connector ids. Needed by wayland-leased-drm:
  // this provider re-opens the card as a plain non-master fd, whose resource
  // view is the WHOLE card -- unlike the lease fd, which the kernel filters to
  // the leased objects. Without the filter a leased display would advertise
  // connectors it does not hold, and `[view.output]` validation would "succeed"
  // against one the compositor still owns. Empty (the default) = no filtering.
  void SetConnectorFilter(std::vector<uint32_t> connector_ids);

 private:
  // The card this provider enumerates (e.g. "/dev/dri/card1").
  std::string device_path_;

  // Connector-id allowlist; empty = enumerate everything on the card.
  std::vector<uint32_t> connector_filter_;

  // Set by SetOutputListener; consumed once the hotplug monitor is wired.
  IOutputListener* listener_ = nullptr;
};

}  // namespace homescreen
