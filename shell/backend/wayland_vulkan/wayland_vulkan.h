/*
 * Copyright 2021-2022 Toyota Connected North America
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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <vector>

// clang-format off
// Include order is load-bearing: the generated protocol client header defines
// the linux_dmabuf_unstable_v1::client traits that the shipped wl/ helpers
// below reference, so it must precede them — clang-format must not reorder.
#include "linux_dmabuf_client.hpp"  // generated (linux_dmabuf_unstable_v1::client)
#include <wl/linux_dmabuf.hpp>      // shipped interface tables + wl_iface()
#include <wl/dmabuf_feedback.hpp>   // wl::DmabufFeedback, wl::FeedbackSnapshot
// clang-format on

#include "vsync/wayland_vsync_provider.h"

// Use vulkan.hpp's convenient proc table and resolver.
#define VULKAN_HPP_NO_EXCEPTIONS 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include "vulkan/vulkan.hpp"

#include "vulkan/vulkan_wayland.h"

#include "backend/backend.h"
#include "config/common.h"
#include "third_party/flutter/shell/platform/embedder/embedder.h"

#if BUILD_COMPOSITOR
#include <memory>
#include <unordered_map>

#include "backend/backing_store_pool.h"
#include "backend/wayland_vulkan/vulkan_backing_store.h"
#include "view/compositor_surface_interface.h"
#include "view/mutation_stack.h"
#include "view/present_layer_sequencer.h"
#endif

class Display;
class TaskRunner;

namespace wl_vulkan {
class DmabufImage;
class WlDmabufBuffer;
class ExplicitSync;
class LayerCompositor;
}  // namespace wl_vulkan

class WaylandVulkanBackend final : public Backend {
 public:
  // @p shell_display may be null in headless / test contexts; the
  // wp_presentation handle for vsync_callback is read from it at
  // construction. The raw wl_display* is the same one returned by
  // shell_display->GetDisplay() — passed separately because Vulkan WSI
  // creation needs it before any Display lookup happens.
  WaylandVulkanBackend(Display* shell_display,
                       wl_display* display,
                       uint32_t width,
                       uint32_t height,
                       bool enable_validation_layers);

  ~WaylandVulkanBackend() override;

  /**
   * @brief Per-backend FlutterVsyncCallback.
   *
   * Returns @c &VsyncTrampoline iff (a) the compositor advertised
   * wp_presentation, (b) the announced clock_id is CLOCK_MONOTONIC,
   * and (c) @c IVI_VK_VSYNC is not @c 0. Otherwise returns nullptr and
   * Flutter falls back to its internal wall-clock scheduler.
   */
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;

  /**
   * @brief Stash the engine handle for the wp_presentation_feedback
   * path. Captured atomically because dispatch may read it from
   * Display's event_thread_ when the listener fires.
   */
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override {
    engine_handle_ = engine;
    if (vsync_) {
      vsync_->SetEngine(engine_handle_, platform_task_runner_);
    }
  }

  /**
   * @brief Stash the platform task runner so @c FlutterEngineOnVsync can
   * be marshalled onto its strand. Required because the listener for
   * @c wp_presentation_feedback.presented fires on Display's event_thread_,
   * and Flutter rejects @c OnVsync from any thread other than the engine's
   * run thread.
   */
  void SetPlatformTaskRunner(TaskRunner* runner) override {
    platform_task_runner_ = runner;
    if (vsync_) {
      vsync_->SetEngine(engine_handle_, platform_task_runner_);
    }
  }

  /**
   * @brief Cancel outstanding wp_presentation_feedback objects so
   * listeners can't fire after the engine has destructed. Called from
   * @c FlutterView::~FlutterView before the engine destructs.
   */
  void StopVsyncMonitor() override;

  /**
   * @brief Resize Flutter engine Window size
   * @param[in] index No use
   * @param[in] engine Pointer to Flutter engine
   * @param[in] width Set window width
   * @param[in] height Set window height
   * @return void
   * @relation
   * wayland
   */
  void Resize(size_t index,
              Engine* engine,
              int32_t width,
              int32_t height) override;

  /**
   * @brief Create Vulkan surface
   * @param[in] index No use
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland
   */
  void CreateSurface(size_t index,
                     wl_surface* surface,
                     int32_t width,
                     int32_t height) override;

  /**
   * @brief Get FlutterRendererConfig
   * @return FlutterRendererConfig
   * @retval Pointer to FlutterRendererConfig
   * @relation
   * wayland
   */
  FlutterRendererConfig GetRenderConfig() override;

  /**
   * @brief Get FlutterCompositor
   * @return FlutterCompositor
   * @retval Pointer to FlutterCompositor
   * @relation
   * wayland
   */
  FlutterCompositor GetCompositorConfig() override;

  // Expose this backend's Vulkan handles so a platform-view plugin can render
  // into a VkImage on the same device Flutter uses (mirrors GetEglContext for
  // EGL plugins). Always available once the device is created.
  bool GetVulkanContext(BackendVulkanContext* out) const override;

  bool TextureMakeCurrent() override;

  bool TextureClearCurrent() override;

#if BUILD_COMPOSITOR
  /// Register a platform-view compositor surface. Called from the platform
  /// thread; must be paired with @c UnregisterCompositorSurface.
  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override;

  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) override;

  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override;

  [[nodiscard]] bool HasDmaBufExport() const { return dma_buf_export_ok_; }
#endif

 private:
  static constexpr VkPresentModeKHR kPreferredPresentMode =
      VK_PRESENT_MODE_FIFO_KHR;

  std::vector<const char*> enabled_instance_extensions_{};
  std::vector<const char*> enabled_device_extensions_{};
  std::vector<const char*> enabled_layer_extensions_{};
  VkInstance instance_{};
  VkSurfaceKHR surface_{};

  VkPhysicalDevice physical_device_{};
  VkPhysicalDeviceFeatures physical_device_features_{};
  VkPhysicalDeviceMemoryProperties physical_device_memory_properties_{};
  VkDevice device_{};
  uint32_t queue_family_index_{};
  VkQueue queue_{};
  // VkQueue access (vkQueueSubmit / vkQueuePresentKHR / vkQueueWaitIdle) must
  // be externally synchronized per the Vulkan spec. The present/get-next-image
  // callbacks run on the Flutter raster thread while init, swapchain
  // (re)creation and the one-shot blit/transfer helpers can touch the same
  // single queue from the platform thread; serialize every queue op on this.
  // The Flutter engine's own submissions on queue_ (raster thread) take this
  // same mutex via the vkQueue* trampolines installed by
  // GetInstanceProcAddressCallback / QueueInterposer (issue #208).
  mutable std::mutex queue_mutex_{};

  bool debugUtilsSupported_{};
  bool enable_validation_layers_;
  bool surfaceSupported_{};
  bool waylandSurfaceSupported_{};

  VkSurfaceFormatKHR surface_format_{};
  VkSwapchainKHR swapchain_{};
  VkCommandPool swapchain_command_pool_{};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkCommandBuffer> present_transition_buffers_;
  // One present-transition semaphore per swapchain image. Per-image (rather
  // than a single shared semaphore) so the non-compositor present path does
  // not need a full vkQueueWaitIdle drain every frame: a given image's
  // semaphore is only reused once that image is re-acquired, which already
  // implies its previous presentation completed.
  std::vector<VkSemaphore> present_transition_semaphores_;
  VkFence image_ready_fence_{};
  uint32_t last_image_index_{};

  bool resize_pending_;

  wl_display* wl_display_;
  uint32_t width_;
  uint32_t height_;

  /**
   * @brief Create Vulkan instance
   * @return void
   * @relation
   * wayland
   */
  void createInstance();

  /**
   * @brief Setup Vulkan debug callback
   * @return void
   * @relation
   * wayland
   */
  void setupDebugMessenger();

  /**
   * @brief Find a compatible Vulkan physical device
   * @return void
   * @relation
   * wayland
   */
  void findPhysicalDevice();

  /**
   * @brief Create Vulkan logical device
   * @return void
   * @relation
   * wayland
   */
  void createLogicalDevice();

  /**
   * @brief Initialize Vulkan swapchain
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool InitializeSwapChain();

  /**
   * @brief Callback to get the next Vulkan image
   * @param[in] user_data Pointer to User data
   * @param[in] frame_info No use
   * @return FlutterVulkanImage
   * @retval Next Vulkan image
   * @relation
   * wayland
   */
  static FlutterVulkanImage GetNextImageCallback(
      void* user_data,
      const FlutterFrameInfo* frame_info);

  /**
   * @brief Callback to queue Vulkan image for presentation
   * @param[in] user_data Pointer to User data
   * @param[in] image No use
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  static bool PresentCallback(void* user_data, const FlutterVulkanImage* image);

  /**
   * @brief Callback to Get instance Process Address
   * @param[in] user_data Pointer to User data
   * @param[in] instance Vulkan instance handle
   * @param[in] procname Process name
   * @return void*
   * @retval Instance Process Address
   * @relation
   * wayland
   */
  static void* GetInstanceProcAddressCallback(
      void* user_data,
      FlutterVulkanInstanceHandle instance,
      const char* procname);

  // Plugin-facing vkGetInstanceProcAddr handed out by GetVulkanContext: like
  // GetInstanceProcAddressCallback (the engine's loader) but in the
  // PFN_vkGetInstanceProcAddr shape, it swaps vkQueue* entry points for the
  // shared-queue locking trampolines so a plugin reusing the device serializes
  // its submits with the engine and this backend (issue #208).
  static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
  PluginInstanceProcAddr(VkInstance instance, const char* procname);

  VkDebugReportCallbackEXT mDebugCallback = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;

  /**
   * @brief Callback to VK_EXT_debug_utils
   * @param[in] severity Bitmask of VkDebugUtilsMessageSeverityFlagBitsEXT
   * @param[in] types No use
   * @param[in] cb_data Structure specifying parameters returned to the callback
   * @param[in] pUserData No use
   * @return VkBool32
   * @retval VK_FALSE Abnormal end
   * @relation
   * wayland
   */
  static VKAPI_ATTR VkBool32

      VKAPI_CALL
      debugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                         VkDebugUtilsMessageTypeFlagsEXT types,
                         const VkDebugUtilsMessengerCallbackDataEXT* cb_data,
                         void* pUserData);

  /**
   * @brief Callback to VK_EXT_debug_report
   * @param[in] flags Bitmask of VkDebugReportFlagBitsEXT
   * @param[in] objectType No use
   * @param[in] object No use
   * @param[in] location No use
   * @param[in] messageCode No use
   * @param[in] pLayerPrefix The name of the component
   * @param[in] pMessage Output message
   * @param[in] pUserData No use
   * @return VkBool32
   * @retval VK_FALSE Abnormal end
   * @relation
   * wayland
   */
  static VKAPI_ATTR VkBool32

      VKAPI_CALL
      debugReportCallback(VkDebugReportFlagsEXT flags,
                          VkDebugReportObjectTypeEXT objectType,
                          uint64_t object,
                          size_t location,
                          int32_t messageCode,
                          const char* pLayerPrefix,
                          const char* pMessage,
                          void* pUserData);

  /**
   * @brief Callback to output information "CollectBackingStore"
   * @param[in] renderer No use
   * @param[in] user_data No use
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * internal
   */
  static bool CollectBackingStore(const FlutterBackingStore* store,
                                  void* user_data);

  /**
   * @brief Callback to output information "CreateBackingStore"
   * @param[in] config No use
   * @param[in] backing_store_out No use
   * @param[in] user_data No use
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * internal
   */
  static bool CreateBackingStore(const FlutterBackingStoreConfig* config,
                                 FlutterBackingStore* backing_store_out,
                                 void* user_data);

  /**
   * @brief Callback to output information "PresentLayers"
   * @param[in] layers No use
   * @param[in] layers_count No use
   * @param[in] user_data No use
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * internal
   */
  static bool PresentLayers(const FlutterLayer** layers,
                            size_t layers_count,
                            void* user_data);

  // Whether the zero-copy dma-buf present path is active (see the DmabufSlot
  // ring below). Declared outside BUILD_COMPOSITOR because CreateSurface reads
  // it to decide whether to create the WSI swapchain, which is compiled on
  // every configuration; it can only ever become true under BUILD_COMPOSITOR.
  bool dmabuf_present_active_{false};

#if BUILD_COMPOSITOR
  bool dma_buf_export_ok_{false};

  BackingStorePool<VulkanBackingStore> m_store_pool;
  PresentLayerSequencer m_sequencer;
  // See the matching comment in WaylandEglBackend — Register/Unregister
  // fire on the platform thread, PresentLayers reads on the rasterizer
  // thread. Held only across map access; plugin callbacks run lock-free.
  mutable std::mutex m_compositor_surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      m_compositor_surfaces;

  // Keeps shared ownership while the engine holds the store.
  std::unordered_map<VulkanBackingStore*, std::shared_ptr<VulkanBackingStore>>
      m_alive_stores;

  bool CreateBackingStoreImpl(const FlutterBackingStoreConfig* config,
                              FlutterBackingStore* store_out);
  bool CollectBackingStoreImpl(const FlutterBackingStore* store);
  bool PresentLayersImpl(const FlutterLayer** layers, size_t count);

  /// Composite one platform-view layer via its ICompositorSurface (texture
  /// route), applying its mutation stack. Shared by the swapchain and dma-buf
  /// present paths. Returns the surface's OnPresent result, or true when the
  /// view takes the subsurface route (no compositor surface registered).
  bool PresentPlatformView(const FlutterLayer* layer);
  /// Sequencer missing-view handler: warn only when a platform view has neither
  /// a subsurface nor a compositor surface registered.
  void WarnMissingPlatformView(FlutterPlatformViewIdentifier id);

#if BUILD_COMPOSITOR
  /// Composite a Vulkan platform view into the dma-buf slot: if the layer's
  /// ICompositorSurface exposes a VkImage (rendered on this device), transition
  /// it to a transfer source and blit it into @p dst at the layer's rect. No-op
  /// for GL / subsurface platform views. Records into @p cmd (already open).
  /// (Overwrite fallback used when the alpha-blend compositor is unavailable.)
  void BlitPlatformViewVulkan(VkCommandBuffer cmd,
                              const FlutterLayer* layer,
                              VkImage dst);

  /// Composite the layer stack into @p slot with src-over alpha blending via
  /// layer_compositor_ (a render pass): each backing store and Vulkan platform
  /// view is sampled and blended in z-order, so a transparent Flutter overlay
  /// over a platform view keeps the view instead of overwriting it to black.
  /// Leaves the slot in GENERAL. Returns the per-view OnPresent success.
  struct DmabufSlot;  // defined below
  bool CompositeLayersBlend(VkCommandBuffer cmd,
                            const FlutterLayer** layers,
                            size_t count,
                            DmabufSlot& slot,
                            uint32_t width,
                            uint32_t height);
#endif

  /// Record + submit a layout transition on a one-shot command buffer.
  void TransitionLayout(VkImage image,
                        VkImageLayout from,
                        VkImageLayout to,
                        VkPipelineStageFlags src_stage,
                        VkPipelineStageFlags dst_stage) const;

  /// Blit @p src onto the given swapchain image, then transition the
  /// swapchain image into PRESENT_SRC_KHR. The command buffer is submitted
  /// and waited on the queue.
  static void BlitStoreToSwapchain(VkCommandBuffer cmd,
                                   VulkanBackingStore& src,
                                   VkImage dst,
                                   int32_t dst_x,
                                   int32_t dst_y,
                                   int32_t dst_w,
                                   int32_t dst_h);

  /// Per-frame pipelining state for compositor mode. We keep one slot per
  /// swapchain image; the slot owns its command buffer and the sync
  /// primitives that gate its reuse. The CPU only waits when about to
  /// reuse a slot — never on the queue at large.
  struct FrameSlot {
    VkCommandBuffer cmd_buffer{VK_NULL_HANDLE};
    VkFence in_flight{VK_NULL_HANDLE};
    VkSemaphore image_available{VK_NULL_HANDLE};
  };

  // Dedicated pool with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT so
  // each slot's cmd buffer can be re-recorded each frame without resetting
  // the whole pool.
  VkCommandPool m_compositor_cmd_pool_{VK_NULL_HANDLE};
  std::vector<FrameSlot> m_compositor_slots_;
  // render_finished is the present-wait semaphore. It must be indexed by
  // swapchain image, NOT by frame slot: a present can't reuse the semaphore
  // until that present completes, which the WSI tracks per image. Indexing it
  // per slot let a slot reuse its semaphore while a prior present on the same
  // image was still pending (VUID-vkQueuePresentKHR-pWaitSemaphores-03268 /
  // MissingAcquireWait).
  std::vector<VkSemaphore> m_compositor_render_finished_;
  size_t m_compositor_current_frame_{0};

  // --- Zero-copy dma-buf present path (bypasses the WSI swapchain) ----------
  // The engine's composited frame is blitted into a modifier-tiled dma-buf
  // image the compositor imports as a wl_buffer and can promote to a hardware
  // plane, committed directly on the wl_surface instead of vkQueuePresentKHR.
  // Gated on the compositor advertising a usable modifier AND IVI_VK_DMABUF;
  // off by default (the swapchain path stays the default until this proves
  // out).
  struct DmabufSlot {
    std::unique_ptr<wl_vulkan::DmabufImage> image;
    std::unique_ptr<wl_vulkan::WlDmabufBuffer> buffer;
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    // Explicit-sync release point for this slot's last commit; polled to know
    // the compositor is done (replaces wl_buffer.release). 0 = never committed.
    uint64_t release_point{0};
  };
  std::vector<uint64_t> dmabuf_modifiers_;  // negotiated set, cached in Create
  VkCommandPool dmabuf_cmd_pool_{VK_NULL_HANDLE};
  std::vector<DmabufSlot> dmabuf_slots_;
  uint32_t dmabuf_ring_w_{0};
  uint32_t dmabuf_ring_h_{0};
  size_t dmabuf_frame_{0};

  // Explicit sync via wp_linux_drm_syncobj: when the compositor advertises the
  // manager and the device supports timeline/external-semaphore-fd, the blit
  // submit signals an acquire timeline the compositor waits on and the slot is
  // reclaimed by polling a release timeline — replacing the per-frame CPU
  // fence. Null when unavailable; the CPU-fence path is used instead.
  std::unique_ptr<wl_vulkan::ExplicitSync> explicit_sync_;

  // Alpha-blend layer compositor for the dma-buf path. When present, the layer
  // stack is composited with src-over blending (a render pass) instead of
  // overwrite blits — required so a platform view under a transparent Flutter
  // overlay is not erased to black. Null falls back to the blit path.
  std::unique_ptr<wl_vulkan::LayerCompositor> layer_compositor_;

  // Refresh pacing for the dma-buf path. The WSI swapchain path is paced by
  // vkQueuePresentKHR blocking at vblank under FIFO; the dma-buf commit is
  // non-blocking, so without this the rasterizer runs free. Each present arms a
  // wl_surface.frame callback (fires on the Display event thread at ~refresh
  // when the surface is visible) and the next present's rasterizer thread waits
  // on it — the same backpressure FIFO gives the swapchain path. A bounded wait
  // degrades an occluded/unmapped surface (no callbacks) to free-running rather
  // than a hang.
  std::mutex frame_mu_;
  std::condition_variable frame_cv_;
  bool frame_ready_{true};  // guarded by frame_mu_; true = may present
  wl_callback* frame_callback_{
      nullptr};  // in-flight, retired on the event thread
  // wl_surface.frame done handler (Display event thread).
  static void OnFrameDone(void* data, wl_callback* cb, uint32_t time);

  // Build (or rebuild after a resize) the ring of dma-buf image+wl_buffer
  // slots at @p w x @p h. Returns false (and clears dmabuf_present_active_) on
  // any allocation failure, so present falls back to the swapchain.
  bool InitDmabufRing(uint32_t w, uint32_t h);
  // Composite the layers into the current slot and commit its wl_buffer on the
  // surface. Returns the present result like PresentLayersImpl.
  bool PresentLayersDmabuf(const FlutterLayer** layers, size_t count);

  /// Allocate the per-slot pool, command buffers, fences, and semaphores.
  /// Idempotent — releases prior state first, so safe to call again after
  /// swapchain recreation.
  void CompositorPipeliningInit();
  void CompositorPipeliningCleanup();
#endif

  // Per-frame cadence profile (IVI_VK_PROFILE=1). Inter-present interval
  // is sampled on CLOCK_MONOTONIC immediately after each successful
  // vkQueuePresentKHR on the rasterizer thread. When the vsync_callback
  // path is engaged (wp_presentation_ != nullptr && clock_compatible_),
  // on_feedback_presented / on_feedback_discarded additionally accumulate
  // flags_or and discarded_frames — matching the wayland_egl FrameProfile
  // exactly so the histograms are directly comparable.
  //
  // Multiple writers (rasterizer for intervals; Display event_thread_ for
  // flags_or / discarded_frames) update *disjoint* fields. The bucketing
  // arithmetic itself is single-writer (rasterizer only).
  struct FrameProfile {
    uint64_t last_present_ns{0};
    uint64_t interval_sum_ns{0};
    uint64_t interval_max_ns{0};
    uint32_t presented_frames{0};
    uint32_t present_failures{0};  // vkQueuePresentKHR != VK_SUCCESS
    uint32_t bucket_60hz{0};       // ≤17ms
    uint32_t bucket_30hz{0};       // 18-33ms
    uint32_t bucket_20hz{0};       // 34-50ms
    uint32_t bucket_slow{0};       // 51-100ms
    uint32_t bucket_idle{0};       // >100ms
    // Populated by on_feedback_presented / on_feedback_discarded when
    // wp_presentation_feedback is wired (vsync_callback engaged).
    // Always zero when the wall-clock fallback is in use.
    uint32_t discarded_frames{0};
    uint32_t flags_or{0};
    // beginFrame-to-present latency: time from the frame_start_time we
    // handed Flutter via OnVsync to the next vkQueuePresentKHR return.
    // Only meaningful when vsync_callback is wired (otherwise
    // last_begin_frame_ns_ is never set and these stay 0).
    uint64_t pipeline_latency_sum_ns{0};
    uint64_t pipeline_latency_max_ns{0};
    uint32_t pipeline_latency_samples{0};
  };
  FrameProfile profile_{};
  FrameProfile session_totals_{};

  // Called from both present paths after vkQueuePresentKHR returns.
  // No-op when IVI_VK_PROFILE is unset. Mutates profile_/session_totals_
  // without locks because the rasterizer thread is the only writer.
  void ProfilePresent(bool ok);

  // wp_presentation vsync now lives in the Display-owned WaylandVsyncProvider;
  // the backend forwards to it. Same pattern as WaylandEglBackend. The
  // engine/runner are re-passed to the provider via SetEngine when both known.
  ivi::WaylandVsyncProvider* vsync_{nullptr};
  FLUTTER_API_SYMBOL(FlutterEngine) engine_handle_ { nullptr };
  TaskRunner* platform_task_runner_{nullptr};

  // Owning Display, kept so CreateSurface can bind the v4 dma-buf feedback
  // (the compositor's scanout-capable modifiers + main device) for the
  // zero-copy present path, independent of Mesa's own WSI dmabuf bind.
  Display* shell_display_{nullptr};
  std::unique_ptr<wl::DmabufFeedback<WaylandVulkanBackend>> dmabuf_feedback_;
  mutable std::mutex fb_mu_;
  wl::FeedbackSnapshot fb_snapshot_;  // guarded by fb_mu_

 public:
  // Fired by the dma-buf feedback helper on every `done`; records the snapshot
  // and logs the scanout-capable modifiers the direct present path can use.
  void OnDmabufFeedback(const wl::FeedbackSnapshot& snap);

 private:
  // frame_start_time most recently handed to FlutterEngineOnVsync.
  // Updated atomically by the OnVsync-posting paths (PostOnVsync's
  // lambda and on_feedback_presented's inline post). Read by
  // ProfilePresent on the rasterizer thread to compute beginFrame-to-
  // present latency. Stays 0 when vsync_callback isn't wired, in which
  // case ProfilePresent skips the latency math.
  //
  // mutable because PostOnVsync is const (mirrors EGL signature) but
  // needs to record the timestamp it hands to the engine.
  mutable std::atomic<uint64_t> last_begin_frame_ns_{0};

  // wl_surface stashed in CreateSurface so wp_presentation_feedback()
  // has a target without re-deriving from VkSurfaceKHR (Vulkan stores
  // the wl_surface internally but doesn't expose it back).
  std::atomic<struct wl_surface*> wl_surface_{nullptr};

  static void VsyncTrampoline(void* user_data, intptr_t baton);
  void SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  // Per-commit feedback request — called from BOTH present paths BEFORE the
  // vkQueuePresentKHR that mints the wl_surface.commit the feedback binds to.
  // Delegates to the provider.
  void RequestPresentationFeedback();
};
