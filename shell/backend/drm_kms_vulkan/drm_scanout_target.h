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

#include <cstdint>
#include <string>
#include <vector>

#include <xf86drmMode.h>

namespace drm_kms_vulkan {

// The KMS scanout target the compositor presents to: a connected connector, its
// CRTC and preferred mode, and the CRTC's primary plane plus the modifiers that
// plane advertises (IN_FORMATS) for a chosen fourcc — the set the backing-store
// modifier negotiation intersects against.
struct ScanoutTarget {
  uint32_t connector_id = 0;
  uint32_t crtc_id = 0;
  uint32_t primary_plane_id = 0;
  uint32_t mode_width = 0;
  uint32_t mode_height = 0;
  drmModeModeInfo mode{};  // full mode for the LayerScene modeset
  std::vector<uint64_t> plane_modifiers;  // for the queried fourcc
};

// Open @p drm_device read-only, find a connected connector + its CRTC + a mode
// + the CRTC's primary plane, and read that plane's IN_FORMATS modifiers for
// @p fourcc. @p mode_spec selects the mode: empty picks the connector's
// preferred mode; "<W>x<H>" or "<W>x<H>@<R>" (R = integer vrefresh Hz) picks
// the first matching mode, falling back to preferred with a warning if none
// matches. Returns false (with @p err set) if no usable display is found. Does
// NOT take DRM master — it only reads KMS state, so it is safe to call before
// the modeset path acquires the device.
bool DiscoverScanoutTarget(const std::string& drm_device,
                           uint32_t fourcc,
                           const std::string& mode_spec,
                           ScanoutTarget& out,
                           std::string& err);

// wayland-leased-drm: probe an already-open fd instead of opening a card.
// @p drm_fd is borrowed -- never closed here.
//
// Not just a convenience. A lease fd's resource view is kernel-filtered to the
// leased objects, so probing through it finds exactly the connector we hold;
// re-opening the node by path would enumerate the whole card and could pick one
// the compositor is still scanning out on. It may also be the only handle we
// have: a leased client uses a lease precisely because it may not be permitted
// to open the DRM node itself.
//
// @p want_connector_id pins the connector (0 = first connected with a mode).
// Pass the LeaseHold's connector_id: a lease may contain several connectors --
// the requested set is only a suggestion and the compositor picks the final set
// -- so without it the probe can modeset the wrong panel and leave the
// requested one dark, silently.
bool DiscoverScanoutTarget(int drm_fd,
                           uint32_t fourcc,
                           const std::string& mode_spec,
                           uint32_t want_connector_id,
                           ScanoutTarget& out,
                           std::string& err);

}  // namespace drm_kms_vulkan
