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

#include <cstdio>
#include <string>

#include "device_caps.h"

int main(int argc, char** argv) {
  const char* dev = argc > 1 ? argv[1] : "/dev/dri/card0";

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
    return 0;
  }

  std::printf("ZERO-COPY GATE: REFUSE  (display_node=%s)\n", dev);
  std::printf("  reason = %s\n", refusal.c_str());
  return 1;
}
