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

#include "backend/backend.h"
#include "bluevk/BlueVK.h"
#include "third_party/flutter/flutter_embedder.h"

class WaylandVulkanBackend : public Backend {
 public:
  WaylandVulkanBackend(struct wl_display* display,
                       uint32_t width,
                       uint32_t height,
                       bool enable_validation_layers);

  ~WaylandVulkanBackend() override;

  MAYBE_UNUSED static void Resize(void* user_data,
                                  size_t index,
                                  Engine* engine,
                                  int32_t width,
                                  int32_t height);

  MAYBE_UNUSED static void CreateSurface(void* user_data,
                                         size_t index,
                                         wl_surface* surface,
                                         int32_t width,
                                         int32_t height);

  FlutterRendererConfig GetRenderConfig() override;

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

  void createInstance();

  void setupDebugMessenger();

  void findPhysicalDevice();

  void createLogicalDevice();

  bool InitializeSwapchain();

  static std::vector<VkLayerProperties> enumerateInstanceLayerProperties();

  static std::vector<VkExtensionProperties>
  enumerateInstanceExtensionProperties();

  static FlutterVulkanImage GetNextImageCallback(
      void* user_data,
      const FlutterFrameInfo* frame_info);

  static bool PresentCallback(void* user_data, const FlutterVulkanImage* image);

  static void* GetInstanceProcAddressCallback(
      void* user_data,
      FlutterVulkanInstanceHandle instance,
      const char* procname);

  VkDebugReportCallbackEXT mDebugCallback = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;

  static VKAPI_ATTR VkBool32

      VKAPI_CALL
      debugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                         VkDebugUtilsMessageTypeFlagsEXT types,
                         const VkDebugUtilsMessengerCallbackDataEXT* cbdata,
                         void* pUserData);

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

  static bool CollectBackingStore(const FlutterBackingStore* renderer,
                                  void* user_data);

  static bool CreateBackingStore(const FlutterBackingStoreConfig* config,
                                 FlutterBackingStore* backing_store_out,
                                 void* user_data);

  static bool PresentLayers(const FlutterLayer** layers,
                            size_t layers_count,
                            void* user_data);
};