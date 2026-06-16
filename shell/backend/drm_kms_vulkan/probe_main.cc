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

// Standalone exerciser for the drm_kms_vulkan zero-copy gate.
//
// VulkanDrmBackend::Create() runs ProbeDeviceCaps() and refuses (logs +
// returns nullptr -> FlutterView exit(EXIT_FAILURE)) when the gate fails. The
// full backend can't easily be run for that check because DrmDisplay opens a
// libseat session first. This driver calls the same ProbeDeviceCaps() in
// isolation so the pass and refuse paths can be observed without bringing up
// libseat / DRM master.
//
// Exit code mirrors the backend's contract: 0 = gate passed, 1 = refused
// (the same outcome that drives FlutterView's exit(EXIT_FAILURE)).
//
// Usage:   drm_kms_vulkan_probe [/dev/dri/cardN]      (default /dev/dri/card0)
// Refuse:  VK_ICD_FILENAMES=<lavapipe icd.json> drm_kms_vulkan_probe   (CPU
// dev)
//          drm_kms_vulkan_probe /dev/dri/does-not-exist                (no
//          node)

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "device_caps.h"
#include "modifier_format.h"

namespace {

// Dump every plane's advertised IN_FORMATS modifiers (decoded) for the common
// 32-bit scanout fourccs, labeled by plane type. Read-only — no DRM master — so
// it is safe to run alongside a live compositor. Tells you whether the display
// can scan out a tiled/compressed layout (AMD DCC, ARM AFBC, Broadcom UIF) and
// on which plane — e.g. on Rockchip VOP2 the AFBC modifiers live on the OVERLAY
// (Cluster) planes while the PRIMARY is LINEAR-only.
void DumpScanoutModifiers(const char* dev) {
  const int fd = ::open(dev, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
  if (planes == nullptr) {
    ::close(fd);
    return;
  }
  std::printf("  scanout modifiers (per-plane IN_FORMATS, read-only):\n");
  for (uint32_t i = 0; i < planes->count_planes; ++i) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(
        fd, planes->planes[i], DRM_MODE_OBJECT_PLANE);
    if (props == nullptr) {
      continue;
    }
    uint64_t plane_type = ~0ULL;
    uint32_t in_formats = 0;
    for (uint32_t p = 0; p < props->count_props; ++p) {
      drmModePropertyRes* pr = drmModeGetProperty(fd, props->props[p]);
      if (pr == nullptr) {
        continue;
      }
      if (std::strcmp(pr->name, "type") == 0) {
        plane_type = props->prop_values[p];
      } else if (std::strcmp(pr->name, "IN_FORMATS") == 0) {
        in_formats = static_cast<uint32_t>(props->prop_values[p]);
      }
      drmModeFreeProperty(pr);
    }
    drmModeFreeObjectProperties(props);
    if (in_formats == 0) {
      continue;
    }
    // On Rockchip VOP2 (and similar) the AFBC-capable Cluster planes are
    // OVERLAY type, not PRIMARY — so report every plane, labeled by type,
    // rather than only the primary.
    const char* type_name = plane_type == DRM_PLANE_TYPE_PRIMARY   ? "PRIMARY"
                            : plane_type == DRM_PLANE_TYPE_OVERLAY ? "OVERLAY"
                            : plane_type == DRM_PLANE_TYPE_CURSOR  ? "CURSOR"
                                                                   : "?";
    drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, in_formats);
    if (blob == nullptr) {
      continue;
    }
    const auto* hdr = static_cast<const drm_format_modifier_blob*>(blob->data);
    const auto base = static_cast<const char*>(blob->data);
    const auto* fmts =
        reinterpret_cast<const uint32_t*>(base + hdr->formats_offset);
    const auto* mods = reinterpret_cast<const drm_format_modifier*>(
        base + hdr->modifiers_offset);
    for (uint32_t f = 0; f < hdr->count_formats; ++f) {
      if (fmts[f] != DRM_FORMAT_XRGB8888 && fmts[f] != DRM_FORMAT_ARGB8888) {
        continue;
      }
      std::printf("    plane %u (%s)  fourcc 0x%08x:", planes->planes[i],
                  type_name, fmts[f]);
      for (uint32_t m = 0; m < hdr->count_modifiers; ++m) {
        if (f < mods[m].offset || f >= mods[m].offset + 64) {
          continue;
        }
        if (((mods[m].formats >> (f - mods[m].offset)) & 1ULL) == 0ULL) {
          continue;
        }
        std::printf(" %s",
                    drm_kms_vulkan::DescribeModifier(mods[m].modifier).c_str());
      }
      std::printf("\n");
    }
    drmModeFreePropertyBlob(blob);
  }
  drmModeFreePlaneResources(planes);
  ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  const char* dev = argc > 1 ? argv[1] : "/dev/dri/card1";

  std::string refusal;
  const drm_kms_vulkan::DeviceCaps caps =
      drm_kms_vulkan::ProbeDeviceCaps(dev, refusal);

  if (caps.zero_copy_supported) {
    std::printf("ZERO-COPY GATE: PASS  (display_node=%s)\n", dev);
    std::printf("  device         = %s\n", caps.device_name.c_str());
    std::printf("  vendor_id      = 0x%04x\n", caps.vendor_id);
    std::printf("  phys_dev_drm   = %s\n",
                caps.has_physical_device_drm ? "yes" : "no");
    std::printf("  timeline_sem   = %s\n",
                caps.has_timeline_semaphore ? "yes" : "no");
    std::printf("  global_priority= %s\n",
                caps.has_global_priority ? "yes" : "no");
    std::printf("  graphics_queues= %u\n", caps.graphics_queue_count);
  } else {
    std::printf("ZERO-COPY GATE: REFUSE  (display_node=%s)\n", dev);
    std::printf("  reason = %s\n", refusal.c_str());
  }

  // The scanout-modifier dump is display-only and read-only, so it is useful
  // regardless of the gate result — on a refuse it still shows what the display
  // wants (e.g. ARM AFBC on Rockchip VOP2) versus a software/incapable Vulkan
  // that can't produce it.
  DumpScanoutModifiers(dev);
  return caps.zero_copy_supported ? 0 : 1;
}
