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

namespace drm_kms_vulkan {

// Capability summary for the DRM/KMS Vulkan scanout backend, filled at device
// bring-up and logged. Everything platform-specific reduces to this struct so
// nothing platform-specific is hardcoded. The struct stays free of Vulkan
// headers so it can be included widely.
struct DeviceCaps {
  // Zero-copy gate: true only when a non-CPU, non-software-rasterizer device
  // exposes the dma-buf import extension set AND a DRM display node is
  // openable. When false the backend refuses at init with a clear diagnostic
  // instead of failing later inside framebuffer import.
  bool zero_copy_supported = false;

  bool has_physical_device_drm = false;  // false on Adreno & Mali blobs
  bool has_timeline_semaphore = false;   // false on Adreno
  bool has_global_priority = false;      // gate a high-priority queue
  bool has_lazy_transient = false;       // transient attachments in tile mem
  bool has_dedicated_transfer_queue =
      false;               // offload copies off the gfx queue
  uint32_t vendor_id = 0;  // fallback match when no DRM node
  uint32_t device_id = 0;
  uint32_t api_version = 0;
  uint32_t graphics_queue_count = 0;  // >1 allows an async composition queue
  uint32_t max_image_2d = 0;          // clamp surface size (4096 on vc4)
  std::string device_name;
  std::string driver_name;
};

// Probe the local Vulkan + DRM stack for the zero-copy scanout gate. On
// success returns caps with @c zero_copy_supported == true. On any failure
// returns @c zero_copy_supported == false and writes a human-readable cause
// into @p refusal_reason.
//
// This is the lightweight check used by the drm_kms_vulkan_probe tool: it
// creates a throwaway instance and no logical device. The backend itself does
// a fuller bring-up (logical device + queues) rather than calling this.
//
// @p display_device is the DRM node intended for scanout (e.g. /dev/dri/card0);
// it must be openable for the gate to pass.
DeviceCaps ProbeDeviceCaps(const std::string& display_device,
                           std::string& refusal_reason);

// Resolve the (major, minor) device numbers of a DRM node path so the backend
// can match a Vulkan physical device's DRM node against the scanout device.
// Returns false (leaving the outputs untouched) when the path cannot be
// stat'd. Lives here so the stat/sysmacros includes stay out of the backend
// translation unit, which pulls in the logging headers.
bool DrmNodeNumber(const std::string& path,
                   unsigned& major_out,
                   unsigned& minor_out);

}  // namespace drm_kms_vulkan
