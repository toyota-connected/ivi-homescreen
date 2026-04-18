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

#include "backend/drm_kms_egl/driver_probe.h"

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "logging.h"

namespace homescreen::driver_probe {
namespace {

// Drivers known to misbehave on overlay planes. Empty today; populate as
// hardware targets are validated. Match is substring-insensitive against
// drmGetVersion()->name (e.g. "amdgpu", "i915", "meson", "rockchip",
// "imx-drm", "vkms", "mali-dp", "panfrost").
constexpr std::array<std::string_view, 0> kOverlayPlaneBlacklist{};

// Drivers known to accept DRM_MODE_ATOMIC_NONBLOCK | ALLOW_MODESET. The
// default is "no" for safety; add drivers here after empirical confirmation.
constexpr std::array<std::string_view, 0> kNonblockModesetAllowlist{};

bool NameMatches(const std::string_view name,
                 const std::array<std::string_view, 0>& /*list*/) {
  // Empty lists for now — kept as the integration point.
  (void)name;
  return false;
}

std::string GetDriverName(const int drm_fd) {
  drmVersionPtr v = drmGetVersion(drm_fd);
  if (!v) {
    return {};
  }
  std::string name(v->name ? v->name : "", v->name_len > 0 ? v->name_len : 0);
  drmFreeVersion(v);
  return name;
}

// Returns 0 if no primary plane found for the CRTC.
uint32_t FindPrimaryPlane(int drm_fd, uint32_t crtc_id) {
  drmModePlaneResPtr planes = drmModeGetPlaneResources(drm_fd);
  if (!planes) {
    return 0;
  }

  // crtc_index bitmask — build from resources.
  drmModeRes* res = drmModeGetResources(drm_fd);
  uint32_t crtc_mask = 0;
  if (res) {
    for (int i = 0; i < res->count_crtcs; ++i) {
      if (res->crtcs[i] == crtc_id) {
        crtc_mask = 1u << i;
        break;
      }
    }
    drmModeFreeResources(res);
  }

  uint32_t primary_id = 0;
  for (uint32_t i = 0; i < planes->count_planes && primary_id == 0; ++i) {
    drmModePlanePtr p = drmModeGetPlane(drm_fd, planes->planes[i]);
    if (!p) {
      continue;
    }
    if ((p->possible_crtcs & crtc_mask) == 0) {
      drmModeFreePlane(p);
      continue;
    }

    // Look up the "type" property on this plane; value DRM_PLANE_TYPE_PRIMARY
    // tells us it's the primary.
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
    if (props) {
      for (uint32_t j = 0; j < props->count_props; ++j) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[j]);
        if (!prop) {
          continue;
        }
        if (std::strcmp(prop->name, "type") == 0 &&
            props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) {
          primary_id = p->plane_id;
        }
        drmModeFreeProperty(prop);
        if (primary_id) {
          break;
        }
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(p);
  }

  drmModeFreePlaneResources(planes);
  return primary_id;
}

// Count overlay planes that can drive this CRTC.
uint32_t CountOverlayPlanes(const int drm_fd, const uint32_t crtc_id) {
  drmModePlaneResPtr planes = drmModeGetPlaneResources(drm_fd);
  if (!planes) {
    return 0;
  }

  drmModeRes* res = drmModeGetResources(drm_fd);
  uint32_t crtc_mask = 0;
  if (res) {
    for (int i = 0; i < res->count_crtcs; ++i) {
      if (res->crtcs[i] == crtc_id) {
        crtc_mask = 1u << i;
        break;
      }
    }
    drmModeFreeResources(res);
  }

  uint32_t overlay_count = 0;
  for (uint32_t i = 0; i < planes->count_planes; ++i) {
    drmModePlanePtr p = drmModeGetPlane(drm_fd, planes->planes[i]);
    if (!p) {
      continue;
    }
    if ((p->possible_crtcs & crtc_mask) == 0) {
      drmModeFreePlane(p);
      continue;
    }
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
    if (props) {
      for (uint32_t j = 0; j < props->count_props; ++j) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[j]);
        if (!prop) {
          continue;
        }
        if (std::strcmp(prop->name, "type") == 0 &&
            props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY) {
          ++overlay_count;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(p);
  }
  drmModeFreePlaneResources(planes);
  return overlay_count;
}

bool PlaneHasProperty(int drm_fd, uint32_t plane_id, const char* name) {
  drmModeObjectPropertiesPtr props =
      drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (!props) {
    return false;
  }
  bool found = false;
  for (uint32_t j = 0; j < props->count_props && !found; ++j) {
    drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[j]);
    if (!prop) {
      continue;
    }
    if (std::strcmp(prop->name, name) == 0) {
      found = true;
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  return found;
}

bool PrimaryPlaneSupportsFormat(int drm_fd,
                                uint32_t plane_id,
                                uint32_t fourcc) {
  drmModePlanePtr p = drmModeGetPlane(drm_fd, plane_id);
  if (!p) {
    return false;
  }
  bool ok = false;
  for (uint32_t i = 0; i < p->count_formats && !ok; ++i) {
    if (p->formats[i] == fourcc) {
      ok = true;
    }
  }
  drmModeFreePlane(p);
  return ok;
}

bool HasAtomicCap(int drm_fd) {
  // DRM_CLIENT_CAP_ATOMIC — we set it in InitDrm, but re-check here
  // because the kernel may lazily reject the cap on some drivers.
  uint64_t cap = 0;
  return drmGetCap(drm_fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) == 0 && cap == 1;
}

bool HasAsyncFlipCap(int drm_fd) {
  uint64_t cap = 0;
  return drmGetCap(drm_fd, DRM_CAP_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
}

}  // namespace

Resolved Resolve(const int drm_fd,
                 const uint32_t crtc_id,
                 const DrmConfig& cfg) {
  Resolved r{};
  r.driver_name = GetDriverName(drm_fd);

  const uint32_t primary_plane = FindPrimaryPlane(drm_fd, crtc_id);
  const uint32_t overlay_count = CountOverlayPlanes(drm_fd, crtc_id);
  const bool atomic_ok = HasAtomicCap(drm_fd);

  // ── atomic_modeset ────────────────────────────────────────────────────
  switch (cfg.modeset) {
    case drm_config::Modeset::kLegacy:
      r.atomic_modeset = false;
      break;
    case drm_config::Modeset::kAtomic:
      r.atomic_modeset = true;
      break;
    case drm_config::Modeset::kAuto:
      r.atomic_modeset = atomic_ok;
      break;
  }

  // ── use_plane_compositor ──────────────────────────────────────────────
  switch (cfg.compositor) {
    case drm_config::Compositor::kGl:
      r.use_plane_compositor = false;
      break;
    case drm_config::Compositor::kPlanes:
      r.use_plane_compositor = true;
      break;
    case drm_config::Compositor::kAuto:
      r.use_plane_compositor =
          atomic_ok && primary_plane != 0 && overlay_count >= 1;
      break;
  }

  // ── allow_nonblock_modeset ────────────────────────────────────────────
  switch (cfg.allow_nonblock_modeset) {
    case drm_config::TriState::kYes:
      r.allow_nonblock_modeset = true;
      break;
    case drm_config::TriState::kNo:
      r.allow_nonblock_modeset = false;
      break;
    case drm_config::TriState::kAuto:
      r.allow_nonblock_modeset =
          NameMatches(r.driver_name, kNonblockModesetAllowlist);
      break;
  }

  // ── primary_format ────────────────────────────────────────────────────
  // Fallback order: caller's preference → XR24 → XB24 → AR24 → AB24 → RG16.
  // Returns 0 when nothing matches; callers must surface that as a hard
  // failure (no point in silently returning an unsupported fourcc — the
  // first KMS commit will just fail anyway, and less obviously).
  constexpr std::array<uint32_t, 5> kFallbackChain = {
      DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888,
      DRM_FORMAT_ABGR8888, DRM_FORMAT_RGB565,
  };
  auto pick_format = [&](uint32_t preferred) -> uint32_t {
    if (primary_plane == 0) {
      return 0;
    }
    if (preferred != 0 &&
        PrimaryPlaneSupportsFormat(drm_fd, primary_plane, preferred)) {
      return preferred;
    }
    for (const uint32_t f : kFallbackChain) {
      if (f == preferred) {
        continue;  // already tried
      }
      if (PrimaryPlaneSupportsFormat(drm_fd, primary_plane, f)) {
        return f;
      }
    }
    return 0;
  };
  switch (cfg.primary_format) {
    case drm_config::PrimaryFormat::kXrgb8888:
      r.primary_format = pick_format(DRM_FORMAT_XRGB8888);
      break;
    case drm_config::PrimaryFormat::kXbgr8888:
      r.primary_format = pick_format(DRM_FORMAT_XBGR8888);
      break;
    case drm_config::PrimaryFormat::kArgb8888:
      r.primary_format = pick_format(DRM_FORMAT_ARGB8888);
      break;
    case drm_config::PrimaryFormat::kAbgr8888:
      r.primary_format = pick_format(DRM_FORMAT_ABGR8888);
      break;
    case drm_config::PrimaryFormat::kRgb565:
      r.primary_format = pick_format(DRM_FORMAT_RGB565);
      break;
    case drm_config::PrimaryFormat::kAuto:
      r.primary_format = pick_format(DRM_FORMAT_XRGB8888);
      break;
  }

  // ── overlay_planes ────────────────────────────────────────────────────
  switch (cfg.overlay_planes) {
    case drm_config::TriState::kYes:
      r.overlay_planes = true;
      break;
    case drm_config::TriState::kNo:
      r.overlay_planes = false;
      break;
    case drm_config::TriState::kAuto:
      r.overlay_planes = overlay_count >= 1 &&
                         !NameMatches(r.driver_name, kOverlayPlaneBlacklist);
      break;
  }

  // ── explicit_sync ─────────────────────────────────────────────────────
  const bool has_fence_props =
      primary_plane != 0 &&
      PlaneHasProperty(drm_fd, primary_plane, "IN_FENCE_FD");
  switch (cfg.explicit_sync) {
    case drm_config::TriState::kYes:
      r.explicit_sync = true;
      break;
    case drm_config::TriState::kNo:
      r.explicit_sync = false;
      break;
    case drm_config::TriState::kAuto:
      r.explicit_sync = has_fence_props;
      break;
  }

  // ── async_flip ────────────────────────────────────────────────────────
  switch (cfg.async_flip) {
    case drm_config::TriState::kYes:
      r.async_flip = true;
      break;
    case drm_config::TriState::kNo:
      r.async_flip = false;
      break;
    case drm_config::TriState::kAuto:
      r.async_flip = HasAsyncFlipCap(drm_fd);
      break;
  }

  return r;
}

void LogResolved(const Resolved& r) {
  spdlog::info(
      "[DrmBackend] driver='{}' compositor={} modeset={} nonblock-modeset={} "
      "primary-fmt=0x{:08x} overlay-planes={} explicit-sync={} async-flip={}",
      r.driver_name.empty() ? "?" : r.driver_name,
      r.use_plane_compositor ? "planes" : "gl",
      r.atomic_modeset ? "atomic" : "legacy",
      r.allow_nonblock_modeset ? "yes" : "no", r.primary_format,
      r.overlay_planes ? "yes" : "no", r.explicit_sync ? "yes" : "no",
      r.async_flip ? "yes" : "no");
}

}  // namespace homescreen::driver_probe