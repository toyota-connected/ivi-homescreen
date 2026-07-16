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

  // Enumerate through @p fd instead of opening device_path_. Needed by
  // wayland-leased-drm, for two reasons:
  //
  //   - Scope. The kernel filters a lease fd's resource view to the leased
  //     objects, so enumerating through it yields exactly the connectors we
  //     hold. Opening the card by path yields the WHOLE card, so a leased
  //     display would advertise connectors the compositor still owns and
  //     `[view.output]` validation would "succeed" against one of them.
  //   - Permission. The point of drm-lease-v1 is that an unprivileged client
  //     can drive a connector *without* being able to open the DRM node. On a
  //     locked-down target the open() here fails with EACCES, enumeration comes
  //     back empty, and a display holding a perfectly good lease advertises no
  //     outputs at all.
  //
  // @p fd is borrowed and must outlive this provider (the display holds the
  // LeaseHold that owns it). -1 (the default) restores the open-by-path path.
  void SetEnumerationFd(int fd);

 private:
  // The card this provider enumerates (e.g. "/dev/dri/card1"). Unused when
  // enumeration_fd_ is set, except for diagnostics.
  std::string device_path_;

  // Borrowed fd to enumerate through; -1 = open device_path_ instead.
  int enumeration_fd_{-1};

  // Set by SetOutputListener; consumed once the hotplug monitor is wired.
  IOutputListener* listener_ = nullptr;
};

}  // namespace homescreen
