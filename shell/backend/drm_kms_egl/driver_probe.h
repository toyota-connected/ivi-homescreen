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

#include <optional>
#include <string>

#include <drm-cxx/display/connector_info.hpp>

#include "backend/drm_kms_egl/drm_backend.h"

namespace homescreen::driver_probe {

// Concrete, post-resolution values. Every field has a single defined
// answer — no more kAuto after Resolve() returns. DrmCompositor and
// DrmBackend read these, not the raw DrmConfig tristates.
struct Resolved {
  // Driver identity — from drmGetVersion. Used for quirk lookups and
  // diagnostics.
  std::string driver_name;

  // Presentation strategy. If false, DrmCompositor uses PresentViaGlFallback.
  bool use_plane_compositor{false};

  // First-frame modeset API.
  bool atomic_modeset{false};

  // Whether the kernel+driver accepts DRM_MODE_ATOMIC_NONBLOCK together
  // with DRM_MODE_ATOMIC_ALLOW_MODESET. Most drivers reject this; kept
  // conservative by default.
  bool allow_nonblock_modeset{false};

  // KMS plane id picked as the primary for this CRTC. Zero when the
  // probe couldn't find one (`use_plane_compositor` is also false in
  // that case). Used by InitGbm to query IN_FORMATS modifiers — some
  // drivers (notably NVIDIA L4T) implement only the modifier-aware
  // gbm_surface_create_with_modifiers entrypoint.
  uint32_t primary_plane{0};

  // GBM / DRM format for primary-plane scanout buffers.
  uint32_t primary_format{0};

  // Format for Flutter-side backing-store BOs. Prefers the alpha sibling
  // of primary_format (XR24→AR24, XB24→AB24) when the primary plane
  // advertises it in IN_FORMATS, so Skia's transparent PV cutouts
  // survive GL blending. Falls back to primary_format on drivers whose
  // primary is alpha-less-only (some imx/rockchip parts), which keeps
  // direct-scanout viable at the cost of opaque overlays.
  uint32_t bs_format{0};

  // Use overlay planes for per-layer scanout (otherwise all layers fold
  // into the composition buffer).
  bool overlay_planes{false};

  // Attach IN_FENCE_FD / read OUT_FENCE_PTR on atomic commits.
  bool explicit_sync{false};

  // Use DRM_MODE_PAGE_FLIP_ASYNC on flip-only commits (tearing updates).
  bool async_flip{false};

  // Parsed EDID for the chosen connector: monitor name, color primaries,
  // HDR static metadata, wide-gamut signaling. nullopt when the kernel
  // exposes no EDID blob or libdisplay-info rejects it. Read-only —
  // consumed by logging today, by HDR signaling work later.
  std::optional<drm::display::ConnectorInfo> connector_info;
};

// Probe the driver behind `drm_fd` and resolve every kAuto field in `cfg`
// against cap queries, plane properties, and a small driver-name quirk
// list. Explicit (non-kAuto) values in `cfg` are honored verbatim.
//
// `connector_id` is used to read the connector's EDID blob; `crtc_id`
// is used to find the primary plane (for format selection). This
// function does not hold state; the returned Resolved is plain data.
Resolved Resolve(int drm_fd,
                 uint32_t connector_id,
                 uint32_t crtc_id,
                 const DrmConfig& cfg);

// One-line log summary of the resolved config, at ihs::log::info. Emitted
// by DrmBackend::Create so the effective knobs are visible at startup.
void LogResolved(const Resolved& r);

}  // namespace homescreen::driver_probe