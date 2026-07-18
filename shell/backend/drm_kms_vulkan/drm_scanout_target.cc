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

#include "drm_scanout_target.h"

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "logging.h"

namespace drm_kms_vulkan {

namespace {

// First value of a named property on a KMS object, or UINT64_MAX.
uint64_t PropValue(int fd, uint32_t obj, uint32_t type, const char* name) {
  drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj, type);
  if (!props) {
    return UINT64_MAX;
  }
  uint64_t v = UINT64_MAX;
  for (uint32_t i = 0; i < props->count_props && v == UINT64_MAX; ++i) {
    drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
    if (p) {
      if (std::strcmp(p->name, name) == 0) {
        v = props->prop_values[i];
      }
      drmModeFreeProperty(p);
    }
  }
  drmModeFreeObjectProperties(props);
  return v;
}

void ReadPlaneModifiers(int fd,
                        uint32_t plane_id,
                        uint32_t fourcc,
                        std::vector<uint64_t>& out) {
  uint64_t blob_id =
      PropValue(fd, plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS");
  if (blob_id == UINT64_MAX) {
    return;
  }
  drmModePropertyBlobRes* blob =
      drmModeGetPropertyBlob(fd, static_cast<uint32_t>(blob_id));
  if (!blob) {
    return;
  }
  const auto* hdr = static_cast<const drm_format_modifier_blob*>(blob->data);
  const auto* base = static_cast<const uint8_t*>(blob->data);
  const auto* fmts =
      reinterpret_cast<const uint32_t*>(base + hdr->formats_offset);
  const auto* mods = reinterpret_cast<const drm_format_modifier*>(
      base + hdr->modifiers_offset);
  for (uint32_t i = 0; i < hdr->count_formats; ++i) {
    if (fmts[i] != fourcc) {
      continue;
    }
    for (uint32_t j = 0; j < hdr->count_modifiers; ++j) {
      if (i < mods[j].offset || i >= mods[j].offset + 64) {
        continue;
      }
      if (mods[j].formats & (1ULL << (i - mods[j].offset))) {
        out.push_back(mods[j].modifier);
      }
    }
  }
  drmModeFreePropertyBlob(blob);
}

}  // namespace

namespace {

// The body of DiscoverScanoutTarget, on an already-open fd. Split out at the
// open() so the same probe runs against a supplied fd -- wayland-leased-drm
// hands it a lease fd, which it must use rather than re-opening the card: the
// kernel filters a lease fd's view to the leased objects, and a leased client
// may have no permission to open the node at all. Never closes @p fd; the
// caller owns it.
bool DiscoverScanoutTargetOnFd(int fd,
                               uint32_t fourcc,
                               const std::string& mode_spec,
                               uint32_t want_connector_id,
                               ScanoutTarget& out,
                               std::string& err) {
  drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  drmModeRes* res = drmModeGetResources(fd);
  if (!res) {
    err = "drmModeGetResources failed";
    return false;
  }

  drmModeConnector* conn = nullptr;
  for (int i = 0; i < res->count_connectors && !conn; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c == nullptr) {
      continue;
    }
    // want_connector_id pins the connector; 0 keeps the historical "first
    // connected with modes" pick. A lease may contain more than one connector
    // -- the requested set is only a suggestion, the compositor picks the final
    // set -- so on a leased fd the heuristic is a coin flip that lights up the
    // wrong panel. It is still correct for the path-opened tiers, which drive
    // whatever card they were pointed at.
    const bool wanted =
        want_connector_id == 0
            ? (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            : c->connector_id == want_connector_id;
    if (wanted) {
      conn = c;
    } else {
      drmModeFreeConnector(c);
    }
  }
  if (conn == nullptr) {
    err = want_connector_id == 0
              ? "no connected connector with a mode"
              : "requested connector " + std::to_string(want_connector_id) +
                    " is not present on this fd";
    drmModeFreeResources(res);
    return false;
  }
  // A pinned connector still has to be usable: pinning says which panel, not
  // whether it is attached. Reported distinctly from "not present" -- for a
  // leased fd the difference is compositor-side (it leased us a dark
  // connector) versus operator-side (wrong --lease-connector).
  if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
    err = "requested connector " + std::to_string(want_connector_id) +
          " is present but not connected with a mode";
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return false;
  }
  out.connector_id = conn->connector_id;
  // Default to the connector's preferred mode (index 0). A mode_spec of
  // "<W>x<H>" or "<W>x<H>@<R>" selects the first matching mode instead (R =
  // integer vrefresh Hz; omitted = any refresh).
  int chosen = 0;
  if (!mode_spec.empty()) {
    unsigned long want_w = 0;
    unsigned long want_h = 0;
    unsigned long want_r = 0;
    char* p = nullptr;
    want_w = std::strtoul(mode_spec.c_str(), &p, 10);
    if (p != nullptr && *p == 'x') {
      want_h = std::strtoul(p + 1, &p, 10);
    }
    if (p != nullptr && *p == '@') {
      want_r = std::strtoul(p + 1, &p, 10);
    }
    int match = -1;
    if (want_w != 0 && want_h != 0) {
      for (int i = 0; i < conn->count_modes; ++i) {
        const drmModeModeInfo& m = conn->modes[i];
        if (m.hdisplay == want_w && m.vdisplay == want_h &&
            (want_r == 0 || m.vrefresh == want_r)) {
          match = i;
          break;
        }
      }
    }
    if (match >= 0) {
      chosen = match;
    } else {
      ihs::log::warn(
          "[drm_scanout] --drm-mode '{}' not found on connector; using "
          "preferred mode {}x{}",
          mode_spec, conn->modes[0].hdisplay, conn->modes[0].vdisplay);
    }
  }
  out.mode = conn->modes[chosen];
  out.mode_width = conn->modes[chosen].hdisplay;
  out.mode_height = conn->modes[chosen].vdisplay;

  drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
  if (enc && enc->crtc_id) {
    out.crtc_id = enc->crtc_id;
  } else if (res->count_crtcs > 0) {
    out.crtc_id = res->crtcs[0];
  }
  if (enc) {
    drmModeFreeEncoder(enc);
  }
  drmModeFreeConnector(conn);
  if (!out.crtc_id) {
    err = "no CRTC for connector";
    drmModeFreeResources(res);
    return false;
  }

  int crtc_index = -1;
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == out.crtc_id) {
      crtc_index = i;
      break;
    }
  }
  drmModeFreeResources(res);
  if (crtc_index < 0) {
    err = "CRTC not found in resources";
    return false;
  }

  drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
  if (!pres) {
    err = "drmModeGetPlaneResources failed";
    return false;
  }
  for (uint32_t i = 0; i < pres->count_planes; ++i) {
    drmModePlane* pl = drmModeGetPlane(fd, pres->planes[i]);
    if (!pl) {
      continue;
    }
    const bool usable = (pl->possible_crtcs & (1u << crtc_index)) != 0;
    drmModeFreePlane(pl);
    if (usable && PropValue(fd, pres->planes[i], DRM_MODE_OBJECT_PLANE,
                            "type") == DRM_PLANE_TYPE_PRIMARY) {
      out.primary_plane_id = pres->planes[i];
      break;
    }
  }
  drmModeFreePlaneResources(pres);
  if (!out.primary_plane_id) {
    err = "no primary plane for CRTC";
    return false;
  }

  ReadPlaneModifiers(fd, out.primary_plane_id, fourcc, out.plane_modifiers);
  return true;
}

}  // namespace

bool DiscoverScanoutTarget(const std::string& drm_device,
                           uint32_t fourcc,
                           const std::string& mode_spec,
                           ScanoutTarget& out,
                           std::string& err) {
  const int fd = ::open(drm_device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    err = "open('" + drm_device + "'): " + std::strerror(errno);
    return false;
  }
  // The path-opened tiers drive the whole card, so there is no connector to pin
  // and the first-connected pick stands.
  const bool ok = DiscoverScanoutTargetOnFd(fd, fourcc, mode_spec,
                                            /*want_connector_id=*/0, out, err);
  ::close(fd);
  return ok;
}

bool DiscoverScanoutTarget(int drm_fd,
                           uint32_t fourcc,
                           const std::string& mode_spec,
                           uint32_t want_connector_id,
                           ScanoutTarget& out,
                           std::string& err) {
  if (drm_fd < 0) {
    err = "DiscoverScanoutTarget: invalid fd";
    return false;
  }
  return DiscoverScanoutTargetOnFd(drm_fd, fourcc, mode_spec, want_connector_id,
                                   out, err);
}

}  // namespace drm_kms_vulkan
