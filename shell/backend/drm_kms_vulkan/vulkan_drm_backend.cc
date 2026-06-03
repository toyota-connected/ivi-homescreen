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

#include "vulkan_drm_backend.h"

#include "device_caps.h"
#include "logging.h"

VulkanDrmBackend::VulkanDrmBackend(uint32_t width,
                                   uint32_t height,
                                   homescreen::DrmSession* session)
    : width_(width), height_(height), session_(session) {}

std::shared_ptr<VulkanDrmBackend> VulkanDrmBackend::Create(
    const std::string& drm_device,
    bool enable_validation,
    homescreen::DrmSession* session) {
  (void)session;

  if (enable_validation) {
    // Validation vendoring + the -d-driven in-process VK_LAYER_PATH setup land
    // in a later phase (plan §7). Note the request so a debug run that expects
    // validation isn't silently unvalidated.
    spdlog::info(
        "[VulkanDrmBackend] validation requested (-d); vendored layer wiring "
        "is not implemented yet (drm_kms_vulkan plan §7)");
  }

  // §4.1 zero-copy gate. On failure this is the clear diagnostic the contract
  // promises instead of a crash deep in drmModeAddFB2WithModifiers.
  std::string refusal;
  const drm_kms_vulkan::DeviceCaps caps =
      drm_kms_vulkan::ProbeDeviceCaps(drm_device, refusal);
  if (!caps.zero_copy_supported) {
    spdlog::critical(
        "[VulkanDrmBackend] zero-copy scanout unsupported on this system; "
        "refusing to start: {}",
        refusal);
    return nullptr;
  }

  spdlog::info(
      "[VulkanDrmBackend] zero-copy gate passed: device='{}' vendor=0x{:04x} "
      "drm_node={} timeline_sem={} global_priority={} gfx_queues={}",
      caps.device_name, caps.vendor_id, caps.has_physical_device_drm,
      caps.has_timeline_semaphore, caps.has_global_priority,
      caps.graphics_queue_count);

  // Phase 0: the gate is the only implemented step. The device bring-up,
  // negotiated-modifier backing store, LayerScene compositor, explicit sync,
  // and session/vsync reuse are Phases 1-6. Refuse rather than hand back a
  // backend that cannot present a frame.
  spdlog::critical(
      "[VulkanDrmBackend] backend is a Phase 0 scaffold — the Vulkan "
      "render/scanout path is not implemented yet; refusing to start. "
      "Use BUILD_BACKEND_DRM_KMS_EGL for a working DRM/KMS backend.");
  return nullptr;
}

// ── Backend interface stubs
// ─────────────────────────────────────────────────── Unreachable while
// Create() refuses; present for the vtable and call sites.

void VulkanDrmBackend::Resize(size_t /*index*/,
                              Engine* /*flutter_engine*/,
                              int32_t /*width*/,
                              int32_t /*height*/) {}

void VulkanDrmBackend::CreateSurface(size_t /*index*/,
                                     struct wl_surface* /*surface*/,
                                     int32_t /*width*/,
                                     int32_t /*height*/) {}

bool VulkanDrmBackend::TextureMakeCurrent() {
  return false;
}

bool VulkanDrmBackend::TextureClearCurrent() {
  return false;
}

FlutterRendererConfig VulkanDrmBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kVulkan;
  config.vulkan.struct_size = sizeof(FlutterVulkanRendererConfig);
  return config;
}

FlutterCompositor VulkanDrmBackend::GetCompositorConfig() {
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  return compositor;
}
