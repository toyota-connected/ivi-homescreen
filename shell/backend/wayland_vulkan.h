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

#include <shell/platform/embedder/embedder.h>
#include "backend/backend.h"
#include "bluevk/BlueVK.h"

class WaylandVulkanBackend : public Backend {
 public:
  WaylandVulkanBackend(struct wl_display* display,
                       uint32_t width,
                       uint32_t height,
                       bool enable_validation_layers);

  ~WaylandVulkanBackend() override;

  /**
   * @brief Resize Flutter engine Window size
   * @param[in] user_data Pointer to User data
   * @param[in] index No use
   * @param[in] engine Pointer to Flutter engine
   * @param[in] width Set window width
   * @param[in] height Set window height
   * @return void
   * @relation
   * wayland
   */
  MAYBE_UNUSED static void Resize(void* user_data,
                                  size_t index,
                                  Engine* engine,
                                  int32_t width,
                                  int32_t height);

  /**
   * @brief Create Vulkan surface
   * @param[in] user_data Pointer to User data
   * @param[in] index No use
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland
   */
  MAYBE_UNUSED static void CreateSurface(void* user_data,
                                         size_t index,
                                         wl_surface* surface,
                                         int32_t width,
                                         int32_t height);

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

 private:
  static constexpr VkPresentModeKHR kPreferredPresentMode =
      VK_PRESENT_MODE_FIFO_KHR;

  struct {
    std::vector<const char*> enabled_instance_extensions;
    std::vector<const char*> enabled_device_extensions;
    std::vector<const char*> enabled_layer_extensions;
    VkInstance instance{};
    VkSurfaceKHR surface{VK_NULL_HANDLE};

    VkPhysicalDevice physical_device{};
    VkPhysicalDeviceFeatures physical_device_features{};
    VkPhysicalDeviceMemoryProperties physical_device_memory_properties{};
    VkDevice device{};
    uint32_t queue_family_index{};
    VkQueue queue{};

    bool validationFeaturesSupported = false;
    bool debugUtilsSupported = false;
    bool debugReportExtensionSupported = false;
    bool debugMarkersSupported{false};
    bool portabilitySubsetSupported{false};
    bool maintenanceSupported[3]{false};

    VkCommandPool swapchain_command_pool{};
    std::vector<VkCommandBuffer> present_transition_buffers;

    VkFence image_ready_fence{};
    VkSemaphore present_transition_semaphore{};

    VkSurfaceFormatKHR surface_format{};
    VkSwapchainKHR swapchain{};
    std::vector<VkImage> swapchain_images;
    uint32_t last_image_index{};

    bool resize_pending = false;
  } state_;

  struct wl_display* wl_display_;
  uint32_t width_;
  uint32_t height_;
  bool enable_validation_layers_;
  bool resize_pending_;

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
  bool InitializeSwapchain();

  /**
   * @brief Get Vulkan layer properties
   * @return std::vector<VkLayerProperties>
   * @retval Vulkan Layer Properties
   * @relation
   * wayland
   */
  static std::vector<VkLayerProperties> enumerateInstanceLayerProperties();

  /**
   * @brief Get Vulkan extension properties
   * @return std::vector<VkExtensionProperties>
   * @retval Vulkan Extension Properties
   * @relation
   * wayland
   */
  static std::vector<VkExtensionProperties>
  enumerateInstanceExtensionProperties();

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
   * @param[in] cbdata Structure specifying parameters returned to the callback
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
                         const VkDebugUtilsMessengerCallbackDataEXT* cbdata,
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
};
