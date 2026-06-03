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
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "backend/backend.h"
#include "backend/drm_kms_vulkan/device_caps.h"

namespace homescreen {
class DrmSession;
}  // namespace homescreen

// DRM/KMS scanout backend that drives the Flutter Vulkan renderer and presents
// on hardware KMS planes via zero-copy dma-buf import, reusing the session /
// modeset / vsync stack from drm_kms_egl below the pixel layer.
//
// Create() brings up the Vulkan instance, selects a physical device that can do
// zero-copy dma-buf scanout (render-node-aware when the device exposes a DRM
// node, vendor/name fallback otherwise), creates the logical device and
// queues, and logs the resolved capabilities. The render and present path is
// not implemented yet, so the backend currently refuses after bring-up rather
// than handing back a backend that cannot present a frame.
class VulkanDrmBackend final : public Backend {
 public:
  // Bring up the device and (for now) refuse. Returns nullptr on every path:
  // the caller treats null as a hard init failure and aborts, exactly as the
  // drm_kms_egl backend does on DrmBackend::Create returning null. @p session
  // may be null when no libseat session is available.
  static std::shared_ptr<VulkanDrmBackend> Create(
      const std::string& drm_device,
      bool enable_validation,
      homescreen::DrmSession* session);

  ~VulkanDrmBackend() override;

  VulkanDrmBackend(const VulkanDrmBackend&) = delete;
  VulkanDrmBackend& operator=(const VulkanDrmBackend&) = delete;

  // ── Backend interface ──────────────────────────────────────────────────────
  // Present/scanout is not wired yet; these satisfy the vtable and the
  // FlutterView call sites.
  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;
  void CreateSurface(size_t index,
                     struct wl_surface* surface,
                     int32_t width,
                     int32_t height) override;
  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;
  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  [[nodiscard]] const drm_kms_vulkan::DeviceCaps& caps() const { return caps_; }

 private:
  VulkanDrmBackend(std::string drm_device,
                   bool enable_validation,
                   homescreen::DrmSession* session);

  // Bring-up steps. Each logs and returns false on failure; refusal_reason
  // carries the cause for gate failures.
  bool BringUp(std::string& refusal_reason);
  bool CreateInstance(std::string& refusal_reason);
  void SetupDebugMessenger();
  bool SelectPhysicalDevice(std::string& refusal_reason);
  bool CreateLogicalDevice(std::string& refusal_reason);
  void PopulateCaps();
  void Teardown();

  std::string drm_device_;
  bool enable_validation_ = false;
  // Reused by the session/vsync glue in a later change; held now so Create()'s
  // signature and the FlutterView wiring are stable.
  [[maybe_unused]] homescreen::DrmSession* session_ = nullptr;  // not owned

  uint32_t width_ = 0;
  uint32_t height_ = 0;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t graphics_queue_family_ = UINT32_MAX;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;

  std::vector<const char*> enabled_instance_extensions_;
  std::vector<const char*> enabled_instance_layers_;
  std::vector<const char*> enabled_device_extensions_;

  drm_kms_vulkan::DeviceCaps caps_;
};
