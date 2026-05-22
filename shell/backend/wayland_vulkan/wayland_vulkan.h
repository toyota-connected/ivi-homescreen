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

#include <cstdint>
#include <vector>

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
#include <mutex>
#include <unordered_map>

#include "backend/backing_store_pool.h"
#include "backend/wayland_vulkan/vulkan_backing_store.h"
#include "view/compositor_surface_interface.h"
#include "view/mutation_stack.h"
#include "view/present_layer_sequencer.h"
#endif

class WaylandVulkanBackend final : public Backend {
 public:
  WaylandVulkanBackend(wl_display* display,
                       uint32_t width,
                       uint32_t height,
                       bool enable_validation_layers);

  ~WaylandVulkanBackend() override;

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

  bool debugUtilsSupported_{};
  bool enable_validation_layers_;
  bool surfaceSupported_{};
  bool waylandSurfaceSupported_{};

  VkSurfaceFormatKHR surface_format_{};
  VkSwapchainKHR swapchain_{};
  VkCommandPool swapchain_command_pool_{};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkCommandBuffer> present_transition_buffers_;
  VkSemaphore present_transition_semaphore_{};
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
  static bool CollectBackingStore(const FlutterBackingStore* renderer,
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
    VkSemaphore render_finished{VK_NULL_HANDLE};
  };

  // Dedicated pool with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT so
  // each slot's cmd buffer can be re-recorded each frame without resetting
  // the whole pool.
  VkCommandPool m_compositor_cmd_pool_{VK_NULL_HANDLE};
  std::vector<FrameSlot> m_compositor_slots_;
  size_t m_compositor_current_frame_{0};

  /// Allocate the per-slot pool, command buffers, fences, and semaphores.
  /// Idempotent — releases prior state first, so safe to call again after
  /// swapchain recreation.
  void CompositorPipeliningInit();
  void CompositorPipeliningCleanup();
#endif

  // Per-frame cadence profile (IVI_VK_PROFILE=1). Measure-only: samples
  // CLOCK_MONOTONIC immediately after each successful vkQueuePresentKHR
  // on the rasterizer thread (the only writer for either present path —
  // PresentCallback for non-compositor, PresentLayersImpl for compositor —
  // and only one of those is active per build). Same shape as the
  // wayland_egl FrameProfile so the bucket histograms are directly
  // comparable. Inter-present interval is the only signal available
  // without wp_presentation_feedback wiring; "discarded" and "flags"
  // are intentionally absent from the Vulkan path.
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
  };
  FrameProfile profile_{};
  FrameProfile session_totals_{};

  // Called from both present paths after vkQueuePresentKHR returns.
  // No-op when IVI_VK_PROFILE is unset. Mutates profile_/session_totals_
  // without locks because the rasterizer thread is the only writer.
  void ProfilePresent(bool ok);
};
