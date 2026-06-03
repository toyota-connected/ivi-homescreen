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
// node, vendor/name fallback otherwise), creates the logical device and queues,
// then opens the DRM device, takes master, discovers the scanout target, and
// builds the LayerScene the present path commits onto. The Flutter compositor
// callbacks back each backing store with an exported modifier VkImage and scan
// it out zero-copy on the primary plane.
class VulkanDrmBackend final : public Backend {
 public:
  // Bring up the device and the present path. Returns nullptr on failure: the
  // caller treats null as a hard init failure and aborts, exactly as the
  // drm_kms_egl backend does on DrmBackend::Create returning null. @p session
  // may be null when no libseat session is available. @p mode_spec selects the
  // scanout mode ("<W>x<H>[@<R>]"); empty uses the connector's preferred mode.
  static std::shared_ptr<VulkanDrmBackend> Create(
      const std::string& drm_device,
      bool enable_validation,
      homescreen::DrmSession* session,
      const std::string& mode_spec);

  ~VulkanDrmBackend() override;

  VulkanDrmBackend(const VulkanDrmBackend&) = delete;
  VulkanDrmBackend& operator=(const VulkanDrmBackend&) = delete;

  // ── Backend interface ──────────────────────────────────────────────────────
  // Present runs through the Flutter compositor callbacks
  // (GetCompositorConfig), so these size/surface/texture entry points are no-op
  // stubs that satisfy the vtable and the FlutterView call sites.
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
                   homescreen::DrmSession* session,
                   std::string mode_spec);

  // Bring-up steps. Each logs and returns false on failure; refusal_reason
  // carries the cause for gate failures.
  bool BringUp(std::string& refusal_reason);
  bool CreateInstance(std::string& refusal_reason);
  void SetupDebugMessenger();
  bool SelectPhysicalDevice(std::string& refusal_reason);
  bool CreateLogicalDevice(std::string& refusal_reason);
  void PopulateCaps();
  void Teardown();

  // Open the DRM device, take master, build the LayerScene for the discovered
  // scanout target, and cache the negotiated modifier set. On success the
  // backend can present and Create() returns it instead of refusing.
  bool SetupCompositor(std::string& err);

  // Root-surface renderer callbacks. The compositor path renders into backing
  // stores and presents via present_layers, so these are never invoked at
  // runtime, but the embedder rejects a Vulkan renderer config that leaves them
  // null.
  static FlutterVulkanImage GetNextImageCb(void* user_data,
                                           const FlutterFrameInfo* frame_info);
  static bool PresentImageCb(void* user_data, const FlutterVulkanImage* image);

  // Flutter compositor callbacks (user_data == this) and their implementations.
  static bool CreateBackingStoreCb(const FlutterBackingStoreConfig* config,
                                   FlutterBackingStore* out,
                                   void* user_data);
  static bool CollectBackingStoreCb(const FlutterBackingStore* store,
                                    void* user_data);
  static bool PresentLayersCb(const FlutterLayer** layers,
                              size_t count,
                              void* user_data);
  bool CreateBackingStoreImpl(const FlutterBackingStoreConfig* config,
                              FlutterBackingStore* out);
  bool CollectBackingStoreImpl(const FlutterBackingStore* store);
  bool PresentLayersImpl(const FlutterLayer** layers, size_t count);

  std::string drm_device_;
  // Scanout mode selector ("<W>x<H>[@<R>]"); empty = connector preferred mode.
  std::string mode_spec_;
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

  // DRM device + LayerScene + backing-store registry. Held behind a pimpl so
  // the drm-cxx scene/device headers stay out of this header (it is included by
  // FlutterView and other GL-free translation units).
  struct CompositorState;
  std::unique_ptr<CompositorState> compositor_;
};
