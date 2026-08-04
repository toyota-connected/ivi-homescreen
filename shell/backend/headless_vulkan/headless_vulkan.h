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
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

#include "backend/backend.h"
#include "ihs/vk_export.h"
#include "vsync/consumer_paced_vsync.h"
#include "vsync/ivsync_provider.h"

class Engine;
class TaskRunner;

// Headless Vulkan backend (initial): boots the Flutter Vulkan renderer with
// no display, no Wayland, no scanout, and paces clear-color frames at a target
// frame rate. It brings up a minimal Vulkan device (instance + physical-device
// select + one graphics queue) and hands the engine a small ring of
// render-target VkImages it composites into; the frames go nowhere yet.
//
// The device is set up so a later change can export those images as
// dma-bufs and hand them across an external-semaphore-synchronized socket to a
// consumer: the external-memory / external-semaphore DEVICE extensions are
// enabled here when the device advertises them (harmless now), and the selected
// device's 16-byte deviceUUID is retained so a future HELLO handshake can name
// the exact GPU. None of that happens yet — PresentCallback only advances
// the round-robin index, and the ConsumerPacedVsyncSource runs free (no
// consumer) so the engine renders at the IVI_HEADLESS_FPS ceiling.
class HeadlessVulkanBackend final : public Backend {
 public:
  HeadlessVulkanBackend(uint32_t width, uint32_t height);
  ~HeadlessVulkanBackend() override;

  HeadlessVulkanBackend(const HeadlessVulkanBackend&) = delete;
  HeadlessVulkanBackend& operator=(const HeadlessVulkanBackend&) = delete;

  // Requested external-memory export mode for the render-target pool, from
  // IVI_VK_EXPORT_MODE (unset => kNone: current non-exportable behavior).
  // Opaque fd is the portable, same-device floor (OPTIMAL tiling); dmabuf
  // exports a DRM-format-modifier image a cross-process/cross-API consumer can
  // import.
  enum class ExportMode { kNone, kOpaqueFd, kDmaBuf };

  // Everything a future handshake needs to re-import one pool image in another
  // process/API. Populated only for the exported images; a plain (non-exported)
  // pool leaves fd == -1 / handle_type == 0. POD by design.
  struct ExportedImage {
    int fd = -1;  // owned by the backend; -1 if not exported
    // Per-frame cross-process GPU sync (OPAQUE_FD exports of binary
    // VkSemaphores; -1 unless the semaphore machinery came up). render_done:
    // the backend signals it once image i is rendered, a consumer waits on it.
    // consumer_done: a consumer signals it when done reading image i, the
    // backend waits before re-rendering. Both are persistent OPAQUE_FD handles
    // a consumer imports once and shares the binary state with the backend.
    int render_done_fd = -1;
    int consumer_done_fd = -1;
    uint64_t alloc_size = 0;
    uint32_t memory_type_index = 0;
    uint32_t drm_fourcc = 0;    // dmabuf only
    uint64_t drm_modifier = 0;  // dmabuf only (DRM_FORMAT_MOD_*)
    uint32_t plane_count = 1;
    uint64_t plane_offset[4] = {0};
    uint64_t plane_pitch[4] = {0};
    uint32_t width = 0, height = 0;
    uint32_t vk_format = 0;  // VkFormat
    uint32_t handle_type =
        0;  // VkExternalMemoryHandleTypeFlagBits used (0=none)
  };

  // ── Backend interface ──────────────────────────────────────────────────────
  // No display surface and no platform-view compositing here, so the
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
  // consumer attached). fps <= 0 leaves Flutter's wall-clock scheduler.
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override;
  void SetPlatformTaskRunner(TaskRunner* runner) override;
  void SetVsyncParked(const bool parked) override { vsync_.SetParked(parked); }

  void StopVsyncMonitor() override;

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }

  // Export accessors for a future cross-process handshake. Nothing consumes
  // these yet — they describe the pool the handshake would advertise. When
  // export is disabled or fell back, export_enabled() is false and every
  // ExportedImage has fd == -1.
  [[nodiscard]] const std::array<ExportedImage, 3>& exported_images() const {
    return exported_images_;
  }
  [[nodiscard]] bool export_enabled() const {
    return active_export_mode_ != ExportMode::kNone;
  }
  // The selected device's 16-byte Vulkan deviceUUID; a HELLO handshake names
  // the exact GPU an opaque-fd import must match.
  [[nodiscard]] const std::array<uint8_t, VK_UUID_SIZE>& device_uuid() const {
    return device_uuid_;
  }

  // ── In-process C ABI (ihs/vk_export.h) ─────────────────────────────────────
  // Backing for the exported `ihs_vk_export_get_api` seam (see
  // vk_export_api.cc). Each is safe to call from the bridge IO thread unless
  // noted; the frame listener fires on the raster thread.

  // Fill @p out with pool-wide capabilities (extent, format, handle type,
  // device UUID, slot count). Never fails; export_active reflects the pool.
  void FillExportCaps(IhsVkExportCaps* out) const;
  // Fill @p out with the current pool descriptors, DUP'ing the three fds per
  // slot (the caller owns/closes the dups). Returns non-zero (leaving @p out
  // zeroed) when there is no exportable pool.
  int FillImageTable(IhsVkExportImageTable* out) const;
  // Register the per-present frame listener (fires on the raster thread; cb ==
  // nullptr clears it). Stored under a mutex.
  void SetExportFrameListener(IhsVkFrameListener cb, void* user);
  // The consumer released @p slot -- the pacing edge for consumer-driven vsync.
  void OnConsumerReleaseFrame(uint32_t slot);
  // Select consumer-driven vs free-run pacing (IHS_VK_PACE_*).
  void SetExportPaceSource(uint32_t src);
  // Inject a pointer event through the engine's normal input path; marshaled to
  // the platform task runner.
  void InjectPointer(const IhsPointerEvent& ev);
  // Rebuild the exported pool at a new extent (consumer-driven resize). Blocks
  // until the raster thread recreates the images + semaphores at the new size,
  // bumps generation_, and the engine is told to relayout. Returns 0 on
  // success, -1 if export is off, the size is unchanged/invalid, or it timed
  // out. Called from the bridge IO thread.
  int RebuildExportPool(uint32_t width, uint32_t height);

 private:
  // Bring-up. Each logs and returns false on failure; a failed InitVulkan
  // leaves the backend inert (GetRenderConfig hands the engine null handles,
  // which it rejects — the same failure surface as the other Vulkan backends).
  bool InitVulkan();
  bool CreateInstance();
  bool SelectPhysicalDevice();
  bool CreateLogicalDevice();
  bool CreateRenderTargets();
  // Pool creators used by CreateRenderTargets. CreateExportablePool honors
  // active_export_mode_ (opaque-fd or dmabuf) and, on any failure, lets the
  // caller fall back to CreatePlainPool (the original non-exportable path) so
  // the backend always boots and paces.
  bool CreatePlainPool();
  bool CreateExportablePool(const std::vector<uint64_t>& allowed_modifiers);
  bool CreatePlainImage(uint32_t index);
  bool CreateExportableImage(uint32_t index,
                             const std::vector<uint64_t>& allowed_modifiers);
  [[nodiscard]] uint32_t FindDeviceLocalMemoryType(uint32_t type_bits) const;
  // Per-frame cross-process GPU sync. Called after the exportable image pool is
  // built (export active only): creates a pair of OPAQUE_FD-exportable binary
  // semaphores per image (render_done / consumer_done), exports their fds, and
  // pre-signals each consumer_done so the first GetNextImage wait passes.
  // Best-effort — logs and skips (leaving the fds -1) if the device cannot
  // export an OPAQUE_FD semaphore; never fails bring-up.
  void CreateExportSemaphores();
  // Create the two exportable binary semaphores per image, export a persistent
  // fd for each (into exported_images_, under export_fd_mutex_), and pre-signal
  // every consumer_done. Returns false on any failure (the caller rolls back).
  // Shared by CreateExportSemaphores (initial) and RebaselineSemaphores (per
  // reconnect); assumes the sync command pool + fence already exist.
  bool InstallExportSemaphores();
  // Recreate the per-frame semaphores from scratch between consumer sessions.
  // Binary semaphores cannot be reset in place, and a detaching consumer leaves
  // render_done/consumer_done in an arbitrary signaled state; recreating gives
  // each new consumer a clean baseline (render_done unsignaled, consumer_done
  // signaled) and fresh export fds. Runs on the raster thread while dormant.
  void RebaselineSemaphores();
  // Destroy the semaphores, close their fds, and destroy the sync command
  // pool + fence. Called by DestroyRenderTargets.
  void DestroyExportSemaphores();
  // The per-frame loopback round-trip (active only when loopback_ &&
  // semaphores_active_), all on graphics_queue_ from the raster thread.
  void WaitConsumerDone(uint32_t k);  // GetNextImage: CPU-wait consumer_done[k]
  void SignalRenderDone(uint32_t k);  // Present: signal render_done[k]
  void LoopbackSelfConsume(
      uint32_t k);  // Present: wait render / signal consumer
  // Destroy the images/memory and close any exported fds. Shared by the
  // export→plain fallback and Teardown.
  void DestroyRenderTargets();
  void Teardown();

  // Perform a pending pool resize on the raster thread at a GetNextImage frame
  // boundary: wait the device idle, recreate the render targets + semaphores at
  // resize_{width,height}_, bump generation_, and unblock RebuildExportPool.
  void PerformResize();
  // Push a new viewport size to the engine so it relayouts at the new extent.
  // The engine metrics API is thread-safe; called from the bridge IO thread.
  void SendWindowMetrics(uint32_t width, uint32_t height);

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

  // Export state. export_mode_ is the mode requested via IVI_VK_EXPORT_MODE;
  // active_export_mode_ is what actually took (kNone after a fallback). The
  // per-image handles/layout a future handshake reads live in exported_images_.
  ExportMode export_mode_{ExportMode::kNone};
  ExportMode active_export_mode_{ExportMode::kNone};
  std::array<ExportedImage, kImageCount> exported_images_{};

  // Per-frame cross-process sync semaphores. render_done_[i] /
  // consumer_done_[i] are binary VkSemaphores exported as OPAQUE_FD (fds stored
  // in exported_images_[i]); semaphores_active_ gates every per-frame op. The
  // sync command pool + reusable fence back the CPU-side consumer_done wait; no
  // command buffers are recorded — the empty submits only carry wait/signal
  // semaphores. IVI_VK_EXPORT_LOOPBACK=1 enables an in-process self-consumer
  // that stands in for the future socket consumer to validate the round-trip.
  bool loopback_{false};
  bool semaphores_active_{false};
  // A real external consumer is attached and driving the pacing (set when the
  // bridge selects consumer-driven pacing, cleared on free-run). Together with
  // loopback_ it decides whether the per-frame render_done/consumer_done
  // handshake runs: with neither, the binary consumer_done has no signaler, so
  // the ops stay dormant and the fds are merely exported. Set from the bridge
  // thread, read on the raster thread.
  std::atomic<bool> export_consumer_active_{false};
  // Latched once per frame in GetNextImage and reused in PresentCallback so a
  // mid-frame attach/detach can never split a frame's wait from its signal
  // (which would leave a binary semaphore double-signaled). Raster-thread only.
  bool frame_sync_active_{false};
  // Set when a consumer detaches; the raster thread recreates the semaphores at
  // the next dormant frame so the following consumer starts from a clean
  // baseline. Read on the raster thread and in FillImageTable (bridge thread).
  std::atomic<bool> sync_rebaseline_pending_{false};
  std::array<VkSemaphore, kImageCount> render_done_{};
  std::array<VkSemaphore, kImageCount> consumer_done_{};
  VkCommandPool sync_command_pool_{VK_NULL_HANDLE};
  VkFence consumer_wait_fence_{VK_NULL_HANDLE};
  // Guards the exported semaphore fds in exported_images_ between the raster
  // thread (RebaselineSemaphores rewrites them) and the bridge thread
  // (FillImageTable dups them). mutable so the const FillImageTable can lock
  // it.
  mutable std::mutex export_fd_mutex_;

  // Consumer-driven pool resize. RebuildExportPool (bridge IO thread) stashes
  // the requested extent, sets resize_pending_, and blocks on resize_cv_ until
  // the raster thread's next GetNextImage runs PerformResize and reports the
  // outcome. FillImageTable also waits out a pending resize so it exports the
  // rebuilt fds. Sizes/flags under resize_mutex_; resize_pending_ is the
  // raster-thread trigger.
  std::atomic<bool> resize_pending_{false};
  std::mutex resize_mutex_;
  std::condition_variable resize_cv_;
  uint32_t resize_width_{0};   // guarded by resize_mutex_
  uint32_t resize_height_{0};  // guarded by resize_mutex_
  bool resize_done_{false};    // guarded by resize_mutex_
  int resize_result_{-1};      // guarded by resize_mutex_

  // Synthetic-vsync pacing. vsync_ holds the baton machinery; the pacer drives
  // it, run in free-run (ceiling-only) mode since there is no consumer.
  ivi::IVsyncProvider vsync_;
  FLUTTER_API_SYMBOL(FlutterEngine) engine_handle_ { nullptr };
  TaskRunner* platform_task_runner_{nullptr};
  std::atomic<bool> vsync_running_{false};
  std::unique_ptr<ivi::ConsumerPacedVsyncSource> pacer_;

  // Exported-pool bookkeeping for the ihs/vk_export.h C ABI. generation_ bumps
  // on every pool rebuild (0 until RebuildExportPool is implemented) and rides
  // the image table so a consumer detects a stale pool. frame_seq_ counts
  // presents delivered to the listener. The listener {cb,user} is set from the
  // bridge thread and read on the raster thread, so it is mutex-guarded.
  uint32_t generation_{0};
  uint32_t frame_seq_{0};
  mutable std::mutex frame_listener_mutex_;
  IhsVkFrameListener frame_listener_{nullptr};
  void* frame_listener_user_{nullptr};
};
