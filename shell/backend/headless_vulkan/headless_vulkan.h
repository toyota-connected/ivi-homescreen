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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "backend/backend.h"
#include "vsync/consumer_paced_vsync.h"
#include "vsync/ivsync_provider.h"

class Engine;
class TaskRunner;

// Headless Vulkan backend (WS-1 stub): boots the Flutter Vulkan renderer with
// no display, no Wayland, no scanout, and paces clear-color frames at a target
// frame rate. It brings up a minimal Vulkan device (instance + physical-device
// select + one graphics queue) and hands the engine a small ring of
// render-target VkImages it composites into; the frames go nowhere yet.
//
// The device is set up so a LATER workstream can export those images as
// dma-bufs and hand them across an external-semaphore-synchronized socket to a
// consumer: the external-memory / external-semaphore DEVICE extensions are
// enabled here when the device advertises them (harmless now), and the selected
// device's 16-byte deviceUUID is retained so a future HELLO handshake can name
// the exact GPU. WS-1 itself does none of that — PresentCallback only advances
// the round-robin index, and the ConsumerPacedVsyncSource runs free (no
// consumer) so the engine renders at the IVI_HEADLESS_FPS ceiling.
class HeadlessVulkanBackend final : public Backend {
 public:
  HeadlessVulkanBackend(uint32_t width, uint32_t height);
  ~HeadlessVulkanBackend() override;

  HeadlessVulkanBackend(const HeadlessVulkanBackend&) = delete;
  HeadlessVulkanBackend& operator=(const HeadlessVulkanBackend&) = delete;

  // ── Backend interface ──────────────────────────────────────────────────────
  // No display surface and no platform-view compositing in WS-1, so the
  // size/surface/texture entry points are trivial stubs that satisfy the vtable
  // and the FlutterView call sites; the engine drives the renderer callbacks
  // (GetRenderConfig) directly.
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

  // Expose the backend's Vulkan device to a platform-view host, the same seam
  // WaylandVulkanBackend / VulkanDrmBackend provide.
  bool GetVulkanContext(BackendVulkanContext* out) const override;

  // Synthetic vsync: without a display there is no vblank, so pace the engine
  // to IVI_HEADLESS_FPS via a ConsumerPacedVsyncSource run in free-run mode (no
  // consumer attached in WS-1). fps <= 0 leaves Flutter's wall-clock scheduler.
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override;
  void SetPlatformTaskRunner(TaskRunner* runner) override;
  void StopVsyncMonitor() override;

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }

 private:
  // Bring-up. Each logs and returns false on failure; a failed InitVulkan
  // leaves the backend inert (GetRenderConfig hands the engine null handles,
  // which it rejects — the same failure surface as the other Vulkan backends).
  bool InitVulkan();
  bool CreateInstance();
  bool SelectPhysicalDevice();
  bool CreateLogicalDevice();
  bool CreateRenderTargets();
  void Teardown();

  // Root-surface renderer callbacks (user_data == the FlutterDesktopEngineState
  // handed to FlutterEngineRun; recovered via BackendOf).
  static void* GetInstanceProcAddressCallback(
      void* user_data,
      FlutterVulkanInstanceHandle instance,
      const char* procname);
  static FlutterVulkanImage GetNextImageCallback(
      void* user_data,
      const FlutterFrameInfo* frame_info);
  static bool PresentCallback(void* user_data, const FlutterVulkanImage* image);

  // The engine's vsync_callback trampoline: parks the baton with the pacer.
  static void VsyncTrampoline(void* user_data, intptr_t baton);
  // Start the pacer once both the engine handle and the runner are wired.
  void StartVsyncIfReady();

  uint32_t width_{0};
  uint32_t height_{0};

  // Vsync ceiling (IVI_HEADLESS_FPS, default 30). 0 disables synthetic vsync.
  uint32_t vsync_period_ns_{0};

  // IVI_VK_DEVICE_UUID: when set, SelectPhysicalDevice matches this 16-byte
  // deviceUUID rather than taking the first non-CPU device.
  bool have_wanted_uuid_{false};
  std::array<uint8_t, VK_UUID_SIZE> wanted_uuid_{};
  // The selected device's deviceUUID, retained for a future export handshake.
  std::array<uint8_t, VK_UUID_SIZE> device_uuid_{};

  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  uint32_t graphics_queue_family_{UINT32_MAX};
  VkQueue graphics_queue_{VK_NULL_HANDLE};

  std::vector<const char*> enabled_instance_extensions_;
  std::vector<const char*> enabled_device_extensions_;

  // Round-robin ring of render targets the engine composites into.
  static constexpr uint32_t kImageCount = 3;
  VkFormat image_format_{VK_FORMAT_R8G8B8A8_UNORM};
  std::array<VkImage, kImageCount> images_{};
  std::array<VkDeviceMemory, kImageCount> image_memory_{};
  uint32_t current_image_{0};

  // Synthetic-vsync pacing. vsync_ holds the baton machinery; the pacer drives
  // it, run in free-run (ceiling-only) mode since WS-1 has no consumer.
  ivi::IVsyncProvider vsync_;
  FLUTTER_API_SYMBOL(FlutterEngine) engine_handle_ { nullptr };
  TaskRunner* platform_task_runner_{nullptr};
  std::atomic<bool> vsync_running_{false};
  std::unique_ptr<ivi::ConsumerPacedVsyncSource> pacer_;
};
