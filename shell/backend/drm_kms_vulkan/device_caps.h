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

// Capability probe result for the zero-copy scanout gate. Only the fields
// Phase 0 needs are populated today; later phases extend this with the
// scanout memory-type index, negotiated modifier, and per-plane layout
// (drm_kms_vulkan plan §4). The struct stays free of Vulkan headers so it can
// be included widely.
struct DeviceCaps {
  // Zero-copy gate (plan §4.1): true only when a non-CPU, non-llvmpipe
  // physical device exposes the dma-buf-import extension set AND the DRM
  // display node is openable.
  bool zero_copy_supported = false;

  bool has_physical_device_drm = false;  // false on Adreno & Mali blobs
  bool has_timeline_semaphore = false;   // false on Adreno -> binary sem floor
  bool has_global_priority = false;      // gate HIGH/REALTIME homescreen queue
  uint32_t vendor_id = 0;                // fallback match when DRM node absent
  uint32_t graphics_queue_count = 0;     // >1 -> async composition blit allowed
  std::string device_name;
  std::string driver_name;
};

// Probe the local Vulkan + DRM stack for the zero-copy scanout gate (plan
// §4.1). On success returns caps with @c zero_copy_supported == true. On any
// failure returns @c zero_copy_supported == false and writes a human-readable
// cause into @p refusal_reason — the diagnostic the backend logs before it
// refuses, instead of crashing deep in @c drmModeAddFB2WithModifiers.
//
// @p display_device is the DRM node intended for scanout (e.g. /dev/dri/card0);
// it must be openable for the gate to pass.
DeviceCaps ProbeDeviceCaps(const std::string& display_device,
                           std::string& refusal_reason);

}  // namespace drm_kms_vulkan
