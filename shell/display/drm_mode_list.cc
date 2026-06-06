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

#include "display/drm_mode_list.h"

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm_mode.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

const char* ConnectorTypeName(const uint32_t type) {
  switch (type) {
    case DRM_MODE_CONNECTOR_Unknown:
      return "Unknown";
    case DRM_MODE_CONNECTOR_VGA:
      return "VGA";
    case DRM_MODE_CONNECTOR_DVII:
      return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:
      return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:
      return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:
      return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:
      return "S-Video";
    case DRM_MODE_CONNECTOR_LVDS:
      return "LVDS";
    case DRM_MODE_CONNECTOR_Component:
      return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:
      return "9PinDIN";
    case DRM_MODE_CONNECTOR_DisplayPort:
      return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:
      return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
      return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:
      return "TV";
    case DRM_MODE_CONNECTOR_eDP:
      return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:
      return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:
      return "DSI";
    case DRM_MODE_CONNECTOR_DPI:
      return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK:
      return "Writeback";
    default:
      return "?";
  }
}

namespace {

const char* PlaneTypeName(const uint64_t type) {
  switch (type) {
    case DRM_PLANE_TYPE_PRIMARY:
      return "PRIMARY";
    case DRM_PLANE_TYPE_OVERLAY:
      return "OVERLAY";
    case DRM_PLANE_TYPE_CURSOR:
      return "CURSOR";
    default:
      return "?";
  }
}

// Report each plane's rotation capability. Rotation is a per-plane KMS property
// (a bitmask whose enum entries are the supported rotate-*/reflect-* bits), not
// a per-mode attribute — so this is the honest place to answer "what can
// rotate". A plane that lists rotate-90/rotate-270 can rotate in hardware,
// subject to the scanout buffer's layout (see the note below).
void PrintPlaneRotation(const int fd) {
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
  if (planes == nullptr) {
    return;
  }
  std::printf("Planes (rotation = hardware capability):\n");
  for (uint32_t i = 0; i < planes->count_planes; ++i) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(
        fd, planes->planes[i], DRM_MODE_OBJECT_PLANE);
    if (props == nullptr) {
      continue;
    }
    uint64_t type = ~0ULL;
    std::string rot;
    bool has_rotation = false;
    for (uint32_t p = 0; p < props->count_props; ++p) {
      drmModePropertyRes* pr = drmModeGetProperty(fd, props->props[p]);
      if (pr == nullptr) {
        continue;
      }
      if (std::strcmp(pr->name, "type") == 0) {
        type = props->prop_values[p];
      } else if (std::strcmp(pr->name, "rotation") == 0) {
        has_rotation = true;
        for (int e = 0; e < pr->count_enums; ++e) {
          if (!rot.empty()) {
            rot += ",";
          }
          rot += pr->enums[e].name;
        }
      }
      drmModeFreeProperty(pr);
    }
    drmModeFreeObjectProperties(props);
    std::printf("  [%u] %-7s  rotation: %s\n", planes->planes[i],
                PlaneTypeName(type),
                has_rotation ? (rot.empty() ? "(none listed)" : rot.c_str())
                             : "not supported (rotate-0 only)");
  }
  drmModeFreePlaneResources(planes);
  std::printf(
      "\nNote: rotation is a per-plane hardware capability, not a per-mode "
      "one.\n"
      "90/270 additionally requires a tiled scanout buffer on some GPUs (e.g.\n"
      "amdgpu rejects 90/270 on a LINEAR framebuffer); --drm-rotation sets "
      "the\n"
      "plane property, but the buffer's modifier must also be compatible.\n");
}

}  // namespace

int PrintDrmModes(const std::string& device) {
  // Read-only: listing modes + plane properties needs no master/RW, so this can
  // run alongside a live compositor (and under a uaccess ACL that only grants
  // read). UNIVERSAL_PLANES is a client cap, settable on an O_RDONLY fd.
  const int fd = ::open(device.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "open(%s): %s\n", device.c_str(),
                 std::strerror(errno));
    return 1;
  }
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr) {
    std::fprintf(stderr, "drmModeGetResources(%s): %s\n", device.c_str(),
                 std::strerror(errno));
    ::close(fd);
    return 1;
  }
  std::printf("Device: %s\n", device.c_str());
  std::printf("Connectors: %d\n\n", res->count_connectors);
  for (int ci = 0; ci < res->count_connectors; ++ci) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[ci]);
    if (c == nullptr) {
      continue;
    }
    const char* state = (c->connection == DRM_MODE_CONNECTED) ? "connected"
                        : (c->connection == DRM_MODE_DISCONNECTED)
                            ? "disconnected"
                            : "unknown";
    std::printf("[%u] %s-%u  %s  modes=%d\n", c->connector_id,
                ConnectorTypeName(c->connector_type), c->connector_type_id,
                state, c->count_modes);
    for (int mi = 0; mi < c->count_modes; ++mi) {
      const drmModeModeInfo& m = c->modes[mi];
      const bool preferred = (m.type & DRM_MODE_TYPE_PREFERRED) != 0;
      std::printf("  %4dx%-4d @ %3dHz  %-20s  %s\n", m.hdisplay, m.vdisplay,
                  m.vrefresh, m.name, preferred ? "[preferred]" : "");
    }
    std::printf("\n");
    drmModeFreeConnector(c);
  }
  PrintPlaneRotation(fd);
  drmModeFreeResources(res);
  ::close(fd);
  return 0;
}
