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

#include "wayland_vulkan.h"

#include <cassert>
#include <cstdlib>
#include <map>
#include <queue>

#include "third_party/flutter/fml/logging.h"

#include "constants.h"
#include "engine.h"

WaylandVulkanBackend::WaylandVulkanBackend(struct wl_display* display,
                                           struct wl_surface* surface,
                                           uint32_t width,
                                           uint32_t height,
                                           bool enable_validation_layers)
    : wl_display_(display),
      wl_surface_(surface),
      width_(width),
      height_(height),
      enable_validation_layers_(enable_validation_layers),
      resize_pending_(false),
      Backend(this, Resize, CreateSurface) {
  if (!bluevk::initialize()) {
    FML_LOG(ERROR) << "BlueVK is unable to load entry points.\n";
    exit(EXIT_FAILURE);
  }
  createInstance();
  setupDebugMessenger();
  createSurface(wl_display_, wl_surface_);
  findPhysicalDevice();
  createLogicalDevice();

  // --------------------------------------------------------------------------
  // Create sync primitives and command pool to use in the render loop
  // callbacks.
  // --------------------------------------------------------------------------

  {
    VkFenceCreateInfo f_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bluevk::vkCreateFence(state_.device, &f_info, nullptr,
                          &state_.image_ready_fence);

    VkSemaphoreCreateInfo s_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    bluevk::vkCreateSemaphore(state_.device, &s_info, nullptr,
                              &state_.present_transition_semaphore);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = state_.queue_family_index,
    };
    bluevk::vkCreateCommandPool(state_.device, &pool_info, nullptr,
                                &state_.swapchain_command_pool);
  }

  // --------------------------------------------------------------------------
  // Create swapchain
  // --------------------------------------------------------------------------

  if (!InitializeSwapchain()) {
    FML_LOG(ERROR) << "Failed to create swapchain.";
    exit(EXIT_FAILURE);
  }
}

FlutterRendererConfig WaylandVulkanBackend::GetRenderConfig() {
  return {
      .type = kVulkan,
      .vulkan{
          .struct_size = sizeof(FlutterRendererConfig),
          .version = VK_MAKE_VERSION(1, 1, 0),
          .instance = state_.instance,
          .physical_device = state_.physical_device,
          .device = state_.device,
          .queue_family_index = state_.queue_family_index,
          .queue = state_.queue,
          .enabled_instance_extension_count =
              state_.enabled_instance_extensions.size(),
          .enabled_instance_extensions =
              state_.enabled_instance_extensions.data(),
          .enabled_device_extension_count =
              state_.enabled_device_extensions.size(),
          .enabled_device_extensions = state_.enabled_device_extensions.data(),
          .get_instance_proc_address_callback = GetInstanceProcAddressCallback,
          .get_next_image_callback = GetNextImageCallback,
          .present_image_callback = PresentCallback,
      }};
}

FlutterCompositor WaylandVulkanBackend::GetCompositorConfig() {
  return {.struct_size = sizeof(FlutterCompositor),
          .user_data = this,
          .create_backing_store_callback = CreateBackingStore,
          .collect_backing_store_callback = CollectBackingStore,
          .present_layers_callback = PresentLayers,
          .avoid_backing_store_cache = true};
}

WaylandVulkanBackend::~WaylandVulkanBackend() {
  if (state_.device != nullptr) {
    if (state_.swapchain_command_pool != nullptr) {
      bluevk::vkDestroyCommandPool(state_.device, state_.swapchain_command_pool,
                                   nullptr);
    }
    if (state_.present_transition_semaphore != nullptr) {
      bluevk::vkDestroySemaphore(state_.device,
                                 state_.present_transition_semaphore, nullptr);
    }
    if (state_.image_ready_fence != nullptr) {
      bluevk::vkDestroyFence(state_.device, state_.image_ready_fence, nullptr);
    }
    bluevk::vkDestroyDevice(state_.device, nullptr);
  }
  if (state_.surface != nullptr) {
    bluevk::vkDestroySurfaceKHR(state_.instance, state_.surface, nullptr);
  }
  if (enable_validation_layers_) {
    if (mDebugCallback) {
      bluevk::vkDestroyDebugReportCallbackEXT(state_.instance, mDebugCallback,
                                              VKALLOC);
    }
    if (mDebugMessenger) {
      bluevk::vkDestroyDebugUtilsMessengerEXT(state_.instance, mDebugMessenger,
                                              VKALLOC);
    }
  }
  if (state_.instance != nullptr) {
    bluevk::vkDestroyInstance(state_.instance, nullptr);
  }
}

void WaylandVulkanBackend::createInstance() {
  auto instance_extensions = enumerateInstanceExtensionProperties();
  for (const auto& l : instance_extensions) {
    FML_DLOG(INFO) << l.extensionName << ", ver: " << l.specVersion;
    if (enable_validation_layers_) {
      if (strcmp(l.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) ==
          0) {
        state_.validationFeaturesSupported = true;
        state_.enabled_instance_extensions.push_back(l.extensionName);
      }
      if (strcmp(l.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
        state_.debugUtilsSupported = true;
        state_.enabled_instance_extensions.push_back(l.extensionName);
      }
      if (strcmp(l.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) {
        state_.debugReportExtensionSupported = true;
        state_.enabled_instance_extensions.push_back(l.extensionName);
      }
    }
  }

  state_.enabled_instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  state_.enabled_instance_extensions.push_back(
      VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

  FML_LOG(INFO) << "Enabling " << state_.enabled_instance_extensions.size()
                << " instance extensions:";
  for (const auto& extension : state_.enabled_instance_extensions) {
    FML_LOG(INFO) << "  - " << extension;
  }

  VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pNext = nullptr,
      .pApplicationName = kApplicationName,
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .pEngineName = "No Engine",
      .engineVersion = VK_MAKE_VERSION(1, 0, 0),
      .apiVersion = VK_MAKE_VERSION(1, 1, 0),
  };

  VkInstanceCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .flags = 0,
      .pApplicationInfo = &app_info,
      .enabledExtensionCount =
          static_cast<uint32_t>(state_.enabled_instance_extensions.size()),
      .ppEnabledExtensionNames = state_.enabled_instance_extensions.data(),
  };

  VkValidationFeaturesEXT features = {};
  VkValidationFeatureEnableEXT enables[] = {
      VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
      // TODO: Enable synchronization validation.
      // VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
  };
  if (state_.validationFeaturesSupported) {
    features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    features.enabledValidationFeatureCount =
        sizeof(enables) / sizeof(enables[0]);
    features.pEnabledValidationFeatures = enables;
    //    info.pNext = &features;
  }

  static constexpr char VK_LAYER_KHRONOS_VALIDATION_NAME[] =
      "VK_LAYER_KHRONOS_validation";

  auto available_layers = enumerateInstanceLayerProperties();
  for (const auto& l : available_layers) {
    // FML_DLOG(INFO) << l.layerName << ", ver: " << l.specVersion;
    if (enable_validation_layers_ &&
        strcmp(l.layerName, VK_LAYER_KHRONOS_VALIDATION_NAME) == 0) {
      state_.enabled_layer_extensions.push_back(
          VK_LAYER_KHRONOS_VALIDATION_NAME);
      break;
    }
  }
  info.enabledLayerCount = state_.enabled_layer_extensions.size();
  info.ppEnabledLayerNames = state_.enabled_layer_extensions.data();

  if (bluevk::vkCreateInstance(&info, nullptr, &state_.instance) !=
      VK_SUCCESS) {
    FML_LOG(ERROR) << "Failed to create Vulkan instance." << std::endl;
    exit(EXIT_FAILURE);
  }

  bluevk::bindInstance(state_.instance);
}

void WaylandVulkanBackend::createSurface(struct wl_display* display,
                                         struct wl_surface* surface) {
  assert(state_.instance != VK_NULL_HANDLE);
  assert(state_.surface == VK_NULL_HANDLE);
  assert(display != nullptr);
  assert(surface != nullptr);

  state_.surface = VK_NULL_HANDLE;

  VkWaylandSurfaceCreateInfoKHR createInfo = {
      .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .display = display,
      .surface = surface};

  VkResult result = bluevk::vkCreateWaylandSurfaceKHR(
      state_.instance, &createInfo, nullptr, &state_.surface);
  if (result != VK_SUCCESS) {
    FML_LOG(ERROR) << "vkCreateWaylandSurfaceKHR failed.";
    assert(false);
  }
}

void WaylandVulkanBackend::setupDebugMessenger() {
  if (!enable_validation_layers_)
    return;

  if (state_.debugUtilsSupported) {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugUtilsCallback,
        .pUserData = nullptr,
    };

    if (bluevk::vkCreateDebugUtilsMessengerEXT(state_.instance, &createInfo,
                                               VKALLOC, &mDebugMessenger) !=
        VK_SUCCESS) {
      FML_LOG(ERROR) << "Unable to create Vulkan debug callback";
    }
  } else if (bluevk::vkCreateDebugReportCallbackEXT) {
    const VkDebugReportCallbackCreateInfoEXT cbinfo = {
        VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT, nullptr,
        VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT,
        debugReportCallback, nullptr};
    if (bluevk::vkCreateDebugReportCallbackEXT(
            state_.instance, &cbinfo, VKALLOC, &mDebugCallback) != VK_SUCCESS) {
      FML_LOG(ERROR) << "Unable to create Vulkan debug callback";
    };
  }
}

void WaylandVulkanBackend::findPhysicalDevice() {
  uint32_t count;
  VkResult result =
      bluevk::vkEnumeratePhysicalDevices(state_.instance, &count, nullptr);
  assert(result == VK_SUCCESS && count > 0);
  std::vector<VkPhysicalDevice> physical_devices(count);
  result = bluevk::vkEnumeratePhysicalDevices(state_.instance, &count,
                                              physical_devices.data());
  assert(result == VK_SUCCESS);

  FML_DLOG(INFO) << "Enumerating " << count << " physical device(s).";

  uint32_t selected_score = 0;
  for (const auto& pdevice : physical_devices) {
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    bluevk::vkGetPhysicalDeviceProperties(pdevice, &properties);
    bluevk::vkGetPhysicalDeviceFeatures(pdevice, &features);

    FML_DLOG(INFO) << "Checking device: " << properties.deviceName;

    uint32_t score = 0;
    std::vector<const char*> supported_extensions;

    uint32_t qfp_count;
    bluevk::vkGetPhysicalDeviceQueueFamilyProperties(pdevice, &qfp_count,
                                                     nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qfp_count);
    bluevk::vkGetPhysicalDeviceQueueFamilyProperties(pdevice, &qfp_count,
                                                     qfp.data());
    std::optional<uint32_t> graphics_queue_family;
    for (uint32_t i = 0; i < qfp.size(); i++) {
      // Only pick graphics queues that can also present to the surface.
      // Graphics queues that can't present are rare if not nonexistent, but
      // the spec allows for this, so check it anyhow.
      VkBool32 surface_present_supported;
      bluevk::vkGetPhysicalDeviceSurfaceSupportKHR(pdevice, i, state_.surface,
                                                   &surface_present_supported);

      if (!graphics_queue_family.has_value() &&
          qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
          surface_present_supported) {
        graphics_queue_family = i;
      }
    }

    // Skip physical devices that don't have a graphics queue.
    if (!graphics_queue_family.has_value()) {
      FML_LOG(INFO) << "  - Skipping due to no suitable graphics queues.";
      continue;
    }

    // Prefer discrete GPUs.
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score += 1 << 30;
    }

    uint32_t extension_count;
    bluevk::vkEnumerateDeviceExtensionProperties(pdevice, nullptr,
                                                 &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    bluevk::vkEnumerateDeviceExtensionProperties(
        pdevice, nullptr, &extension_count, available_extensions.data());

    bool supports_swapchain = false;
    for (const auto& available_extension : available_extensions) {
      FML_DLOG(INFO) << available_extension.extensionName
                     << ", ver: " << available_extension.specVersion;
      if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                 available_extension.extensionName) == 0) {
        supports_swapchain = true;
        supported_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      }
      // The spec requires VK_KHR_portability_subset be enabled whenever it's
      // available on a device. It's present on compatibility ICDs like
      // MoltenVK.
      else if (strcmp(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
                      available_extension.extensionName) == 0) {
        supported_extensions.push_back(
            VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
      }
      // Prefer GPUs that support VK_KHR_get_memory_requirements2.
      else if (strcmp(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                      available_extension.extensionName) == 0) {
        score += 1 << 29;
        supported_extensions.push_back(
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
      } else if (strcmp(VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
                        available_extension.extensionName) == 0) {
        state_.debugMarkersSupported = true;
      } else if (strcmp(VK_KHR_MAINTENANCE1_EXTENSION_NAME,
                        available_extension.extensionName) == 0) {
        state_.maintenanceSupported[0] = true;
        supported_extensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
      } else if (strcmp(VK_KHR_MAINTENANCE2_EXTENSION_NAME,
                        available_extension.extensionName) == 0) {
        state_.maintenanceSupported[1] = true;
        supported_extensions.push_back(VK_KHR_MAINTENANCE2_EXTENSION_NAME);
      } else if (strcmp(VK_KHR_MAINTENANCE3_EXTENSION_NAME,
                        available_extension.extensionName) == 0) {
        state_.maintenanceSupported[2] = true;
        supported_extensions.push_back(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
      }
    }

    // Skip physical devices that don't have swapchain support.
    if (!supports_swapchain) {
      FML_DLOG(INFO) << "  - Skipping due to lack of swapchain support.";
      continue;
    }

    // Prefer GPUs with larger max texture sizes.
    score += properties.limits.maxImageDimension2D;

    if (selected_score < score) {
      FML_DLOG(INFO) << "  - This is the best device so far. Score: 0x"
                     << std::hex << score << std::dec;

      selected_score = score;
      state_.physical_device = pdevice;
      state_.enabled_device_extensions = supported_extensions;
      state_.queue_family_index =
          graphics_queue_family.value_or(std::numeric_limits<uint32_t>::max());

      // Bingo, we finally found a physical device that supports everything we
      // need.
      bluevk::vkGetPhysicalDeviceFeatures(pdevice,
                                          &state_.physical_device_features);
      bluevk::vkGetPhysicalDeviceMemoryProperties(
          pdevice, &state_.physical_device_memory_properties);

      // Print some driver or MoltenVK information if it is available.
      if (bluevk::vkGetPhysicalDeviceProperties2KHR) {
        VkPhysicalDeviceDriverProperties driverProperties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 physicalDeviceProperties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driverProperties,
        };
        bluevk::vkGetPhysicalDeviceProperties2KHR(state_.physical_device,
                                                  &physicalDeviceProperties2);
        FML_LOG(INFO) << "Vulkan device driver: " << driverProperties.driverName
                      << " " << driverProperties.driverInfo;
      }

      // Print out some properties of the GPU for diagnostic purposes.
      //
      const uint32_t driverVersion = properties.driverVersion;
      const uint32_t vendorID = properties.vendorID;
      const uint32_t deviceID = properties.deviceID;
      const int major = VK_VERSION_MAJOR(properties.apiVersion);
      const int minor = VK_VERSION_MINOR(properties.apiVersion);
      FML_LOG(INFO) << "vendor " << std::hex << vendorID << ", "
                    << "device " << deviceID << ", "
                    << "driver " << driverVersion << ", " << std::dec << "api "
                    << major << "." << minor;
      break;
    }
  }

  if (state_.physical_device == nullptr) {
    FML_LOG(ERROR) << "Failed to find a compatible Vulkan physical device."
                   << std::endl;
    exit(EXIT_FAILURE);
  }
}

void WaylandVulkanBackend::createLogicalDevice() {
  FML_DLOG(INFO) << "Enabling " << state_.enabled_device_extensions.size()
                 << " device extensions:";
  for (const char* extension : state_.enabled_device_extensions) {
    FML_DLOG(INFO) << "  - " << extension;
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo graphics_queue = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = state_.queue_family_index,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };

  VkPhysicalDeviceFeatures device_features = {};
  VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &graphics_queue,
      .enabledExtensionCount =
          static_cast<uint32_t>(state_.enabled_device_extensions.size()),
      .ppEnabledExtensionNames = state_.enabled_device_extensions.data(),
      .pEnabledFeatures = &device_features,
  };

  VkResult result = bluevk::vkCreateDevice(state_.physical_device, &device_info,
                                           nullptr, &state_.device);
  if (result != VK_SUCCESS) {
    state_.device = VK_NULL_HANDLE;
    FML_LOG(ERROR) << "Failed to create Vulkan logical device: " << result;
    exit(EXIT_FAILURE);
  }

  bluevk::vkGetDeviceQueue(state_.device, state_.queue_family_index, 0,
                           &state_.queue);
}

bool WaylandVulkanBackend::InitializeSwapchain() {
  if (resize_pending_) {
    resize_pending_ = false;
    bluevk::vkDestroySwapchainKHR(state_.device, state_.swapchain, nullptr);

    bluevk::vkQueueWaitIdle(state_.queue);
    bluevk::vkResetCommandPool(state_.device, state_.swapchain_command_pool,
                               VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
  }

  // --------------------------------------------------------------------------
  // Choose an image format that can be presented to the surface, preferring
  // the common BGRA+sRGB if available.
  // --------------------------------------------------------------------------

  uint32_t format_count;
  bluevk::vkGetPhysicalDeviceSurfaceFormatsKHR(
      state_.physical_device, state_.surface, &format_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  bluevk::vkGetPhysicalDeviceSurfaceFormatsKHR(
      state_.physical_device, state_.surface, &format_count, formats.data());
  assert(!formats.empty());  // Shouldn't be possible.

  state_.surface_format = formats[0];
  for (const auto& format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      state_.surface_format = format;
    }
  }

  // --------------------------------------------------------------------------
  // Choose the presentable image size that's as close as possible to the
  // window size.
  // --------------------------------------------------------------------------

  VkExtent2D clientSize;

  VkSurfaceCapabilitiesKHR surface_capabilities;
  bluevk::vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      state_.physical_device, state_.surface, &surface_capabilities);

  if (surface_capabilities.currentExtent.width != UINT32_MAX) {
    // If the surface reports a specific extent, we must use it.
    clientSize = surface_capabilities.currentExtent;
  } else {
    VkExtent2D actual_extent = {
        .width = width_,
        .height = height_,
    };
    clientSize.width =
        std::max(surface_capabilities.minImageExtent.width,
                 std::min(surface_capabilities.maxImageExtent.width,
                          actual_extent.width));
    clientSize.height =
        std::max(surface_capabilities.minImageExtent.height,
                 std::min(surface_capabilities.maxImageExtent.height,
                          actual_extent.height));
  }

  // --------------------------------------------------------------------------
  // Desired image count
  // --------------------------------------------------------------------------

  const uint32_t maxImageCount = surface_capabilities.maxImageCount;
  const uint32_t minImageCount = surface_capabilities.minImageCount;
  uint32_t desiredImageCount = minImageCount + 1;

  // According to section 30.5 of VK 1.1, maxImageCount of zero means "that
  // there is no limit on the number of images, though there may be limits
  // related to the total amount of memory used by presentable images."
  if (maxImageCount != 0 && desiredImageCount > maxImageCount) {
    FML_LOG(ERROR) << "Swap chain does not support " << desiredImageCount
                   << " images.";
    desiredImageCount = surface_capabilities.minImageCount;
  }

  // --------------------------------------------------------------------------
  // Choose the present mode.
  // --------------------------------------------------------------------------

  uint32_t mode_count;
  bluevk::vkGetPhysicalDeviceSurfacePresentModesKHR(
      state_.physical_device, state_.surface, &mode_count, nullptr);
  std::vector<VkPresentModeKHR> modes(mode_count);
  bluevk::vkGetPhysicalDeviceSurfacePresentModesKHR(
      state_.physical_device, state_.surface, &mode_count, modes.data());
  assert(!formats.empty());  // Shouldn't be possible.

  // If the preferred mode isn't available, just choose the first one.
  VkPresentModeKHR present_mode = modes[0];
  for (const auto& mode : modes) {
    if (mode == kPreferredPresentMode) {
      present_mode = mode;
      break;
    }
  }

  // --------------------------------------------------------------------------
  // Create the swapchain.
  // --------------------------------------------------------------------------

  const VkCompositeAlphaFlagBitsKHR compositeAlpha =
      (surface_capabilities.supportedCompositeAlpha &
       VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
          ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
          : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

  VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .pNext = nullptr,
      .surface = state_.surface,
      .minImageCount = desiredImageCount,
      .imageFormat = state_.surface_format.format,
      .imageColorSpace = state_.surface_format.colorSpace,
      .imageExtent = clientSize,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |  // Allows use as a blit
                                                       // destination.
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // Allows use as a blit
                                                      // source (for readPixels)
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
      .preTransform = surface_capabilities.currentTransform,
      .compositeAlpha = compositeAlpha,
      .presentMode = present_mode,
      .clipped = VK_TRUE,
      .oldSwapchain = VK_NULL_HANDLE,
  };
  VkResult result = bluevk::vkCreateSwapchainKHR(state_.device, &info, VKALLOC,
                                                 &state_.swapchain);
  if (result != VK_SUCCESS) {
    FML_LOG(ERROR) << "vkGetSwapchainImagesKHR(): " << result;
    return false;
  }

  // --------------------------------------------------------------------------
  // Fetch swapchain images
  // --------------------------------------------------------------------------

  uint32_t image_count;
  bluevk::vkGetSwapchainImagesKHR(state_.device, state_.swapchain, &image_count,
                                  nullptr);
  state_.swapchain_images.reserve(image_count);
  state_.swapchain_images.resize(image_count);
  bluevk::vkGetSwapchainImagesKHR(state_.device, state_.swapchain, &image_count,
                                  state_.swapchain_images.data());

  // --------------------------------------------------------------------------
  // Record a command buffer for each of the images to be executed prior to
  // presenting.
  // --------------------------------------------------------------------------

  state_.present_transition_buffers.resize(state_.swapchain_images.size());

  VkCommandBufferAllocateInfo buffers_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = state_.swapchain_command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount =
          static_cast<uint32_t>(state_.present_transition_buffers.size()),
  };
  bluevk::vkAllocateCommandBuffers(state_.device, &buffers_info,
                                   state_.present_transition_buffers.data());

  for (size_t i = 0; i < state_.swapchain_images.size(); i++) {
    auto image = state_.swapchain_images[i];
    auto buffer = state_.present_transition_buffers[i];

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bluevk::vkBeginCommandBuffer(buffer, &begin_info);

    // Filament Engine hands back the image after writing to it
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }};
    bluevk::vkCmdPipelineBarrier(
        buffer,                                         // commandBuffer
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  // srcStageMask
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,           // dstStageMask
        0,                                              // dependencyFlags
        0,                                              // memoryBarrierCount
        nullptr,                                        // pMemoryBarriers
        0,        // bufferMemoryBarrierCount
        nullptr,  // pBufferMemoryBarriers
        1,        // imageMemoryBarrierCount
        &barrier  // pImageMemoryBarriers
    );

    bluevk::vkEndCommandBuffer(buffer);
  }

  return true;
}

std::vector<VkLayerProperties>
WaylandVulkanBackend::enumerateInstanceLayerProperties() {
  std::vector<VkLayerProperties> properties;
  uint32_t propertyCount;
  VkResult res;

  do {
    res = bluevk::vkEnumerateInstanceLayerProperties(&propertyCount, nullptr);
    if ((res == VK_SUCCESS) && propertyCount) {
      properties.resize(propertyCount);
      res = bluevk::vkEnumerateInstanceLayerProperties(
          &propertyCount,
          reinterpret_cast<VkLayerProperties*>(properties.data()));
    }
  } while (res == VK_INCOMPLETE);

  if (res == VK_SUCCESS) {
    assert(propertyCount <= properties.size());
    properties.resize(propertyCount);
  }
  return properties;
}

std::vector<VkExtensionProperties>
WaylandVulkanBackend::enumerateInstanceExtensionProperties() {
  std::vector<VkExtensionProperties> properties;
  uint32_t propertyCount;
  VkResult res;
  do {
    res = bluevk::vkEnumerateInstanceExtensionProperties(
        nullptr, &propertyCount, nullptr);
    if ((res == VK_SUCCESS) && propertyCount) {
      properties.resize(propertyCount);
      res = bluevk::vkEnumerateInstanceExtensionProperties(
          nullptr, &propertyCount,
          reinterpret_cast<VkExtensionProperties*>(properties.data()));
    }
  } while (res == VK_INCOMPLETE);
  if (res == VK_SUCCESS) {
    assert(propertyCount <= properties.size());
    properties.resize(propertyCount);
  }
  return properties;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
WaylandVulkanBackend::debugReportCallback(VkDebugReportFlagsEXT flags,
                                          VkDebugReportObjectTypeEXT objectType,
                                          uint64_t object,
                                          size_t location,
                                          int32_t messageCode,
                                          const char* pLayerPrefix,
                                          const char* pMessage,
                                          void* pUserData) {
  if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
    FML_LOG(ERROR) << "VULKAN ERROR: (" << pLayerPrefix << ") " << pMessage;
  } else {
    FML_LOG(WARNING) << "VULKAN WARNING: (" << pLayerPrefix << ") " << pMessage;
  }
  return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL WaylandVulkanBackend::debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* cbdata,
    void* pUserData) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    FML_LOG(ERROR) << "VULKAN ERROR: (" << cbdata->pMessageIdName << ") "
                   << cbdata->pMessage;
  } else {
    // TODO: emit best practices warnings about aggressive pipeline barriers.
    if (strstr(cbdata->pMessage, "ALL_GRAPHICS_BIT") ||
        strstr(cbdata->pMessage, "ALL_COMMANDS_BIT")) {
      return VK_FALSE;
    }
    FML_LOG(WARNING) << "VULKAN WARNING: (" << cbdata->pMessageIdName << ") "
                     << cbdata->pMessage;
  }
  return VK_FALSE;
}

FlutterVulkanImage WaylandVulkanBackend::GetNextImageCallback(
    void* user_data,
    const FlutterFrameInfo* frame_info) {
  (void)frame_info;
  // If the framebuffer has been resized, discard the swapchain and create
  // a new one.
  auto e = reinterpret_cast<Engine*>(user_data);
  auto b = reinterpret_cast<WaylandVulkanBackend*>(e->GetBackend());
  if (b->state_.resize_pending) {
    b->InitializeSwapchain();
  }

  bluevk::vkAcquireNextImageKHR(
      b->state_.device, b->state_.swapchain, UINT64_MAX, nullptr,
      b->state_.image_ready_fence, &b->state_.last_image_index);

  // Flutter Engine expects the image to be available for transitioning and
  // attaching immediately, and so we need to force a host sync here before
  // returning.
  bluevk::vkWaitForFences(b->state_.device, 1, &b->state_.image_ready_fence,
                          true, UINT64_MAX);
  bluevk::vkResetFences(b->state_.device, 1, &b->state_.image_ready_fence);

  return {
      .struct_size = sizeof(FlutterVulkanImage),
      .image = reinterpret_cast<uint64_t>(
          b->state_.swapchain_images[b->state_.last_image_index]),
      .format = b->state_.surface_format.format,
  };
}

bool WaylandVulkanBackend::PresentCallback(void* user_data,
                                           const FlutterVulkanImage* image) {
  (void)image;
  auto e = reinterpret_cast<Engine*>(user_data);
  auto b = reinterpret_cast<WaylandVulkanBackend*>(e->GetBackend());
  VkPipelineStageFlags stage_flags =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 0,
      .pWaitSemaphores = nullptr,
      .pWaitDstStageMask = &stage_flags,
      .commandBufferCount = 1,
      .pCommandBuffers =
          &b->state_.present_transition_buffers[b->state_.last_image_index],
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &b->state_.present_transition_semaphore,
  };
  bluevk::vkQueueSubmit(b->state_.queue, 1, &submit_info, nullptr);

  VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &b->state_.present_transition_semaphore,
      .swapchainCount = 1,
      .pSwapchains = &b->state_.swapchain,
      .pImageIndices = &b->state_.last_image_index,
  };
  VkResult result = bluevk::vkQueuePresentKHR(b->state_.queue, &present_info);

  // If the swapchain is no longer compatible with the surface, discard the
  // swapchain and create a new one.
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    b->InitializeSwapchain();
  }
  bluevk::vkQueueWaitIdle(b->state_.queue);

  return result == VK_SUCCESS;
}

void* WaylandVulkanBackend::GetInstanceProcAddressCallback(
    void* user_data,
    FlutterVulkanInstanceHandle instance,
    const char* procname) {
  (void)user_data;
  auto* proc = bluevk::vkGetInstanceProcAddr(
      reinterpret_cast<VkInstance>(instance), procname);
  return reinterpret_cast<void*>(proc);
}

void WaylandVulkanBackend::Resize(void* user_data,
                                  size_t index,
                                  Engine* engine,
                                  int32_t width,
                                  int32_t height) {
  (void)index;
  auto b = reinterpret_cast<WaylandVulkanBackend*>(user_data);
  if (b->width_ != width || b->height_ != height) {
    b->resize_pending_ = true;
    b->width_ = width;
    b->height_ = height;
    if (engine) {
      auto result = engine->SetWindowSize(height, width);
      if (result != kSuccess) {
        FML_LOG(ERROR) << "Failed to set Flutter Engine Window Size";
      }
    }
  }
}

void WaylandVulkanBackend::CreateSurface(void* user_data,
                                         size_t index,
                                         wl_surface* surface,
                                         int32_t width,
                                         int32_t height) {
  (void)user_data;
  (void)index;
  (void)surface;
  (void)width;
  (void)height;
  FML_DLOG(INFO) << "CreateSurface";
}

bool WaylandVulkanBackend::CollectBackingStore(
    const FlutterBackingStore* renderer,
    void* user_data) {
  FML_DLOG(INFO) << "CollectBackingStore";
  return false;
}

bool WaylandVulkanBackend::CreateBackingStore(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out,
    void* user_data) {
  FML_DLOG(INFO) << "CreateBackingStore";
#if 0
  auto surface_size = SkISize::Make(config->size.width, config->size.height);
  TestVulkanImage* test_image = new TestVulkanImage(
      std::move(test_vulkan_context_->CreateImage(surface_size).value()));

  GrVkImageInfo image_info = {
      .fImage = test_image->GetImage(),
      .fImageTiling = VK_IMAGE_TILING_OPTIMAL,
      .fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .fFormat = VK_FORMAT_R8G8B8A8_UNORM,
      .fImageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
      .fSampleCount = 1,
      .fLevelCount = 1,
  };
  GrBackendTexture backend_texture(surface_size.width(), surface_size.height(),
                                   image_info);

  SkSurfaceProps surface_properties(0, kUnknown_SkPixelGeometry);

  SkSurface::TextureReleaseProc release_vktexture = [](void* user_data) {
    delete reinterpret_cast<TestVulkanImage*>(user_data);
  };

  sk_sp<SkSurface> surface = SkSurface::MakeFromBackendTexture(
      context_.get(),            // context
      backend_texture,           // back-end texture
      kTopLeft_GrSurfaceOrigin,  // surface origin
      1,                         // sample count
      kRGBA_8888_SkColorType,    // color type
      SkColorSpace::MakeSRGB(),  // color space
      &surface_properties,       // surface properties
      release_vktexture,         // texture release proc
      test_image                 // release context
  );

  if (!surface) {
    FML_LOG(ERROR) << "Could not create Skia surface from Vulkan image.";
    return false;
  }
  backing_store_out->type = kFlutterBackingStoreTypeVulkan;

  auto* image = new FlutterVulkanImage();
  image->image = reinterpret_cast<uint64_t>(image_info.fImage);
  image->format = VK_FORMAT_R8G8B8A8_UNORM;
  backing_store_out->vulkan.image = image;

  // Collect all allocated resources in the destruction_callback.
  {
    UserData* user_data = new UserData();
    user_data->image = image;
    user_data->surface = surface.get();

    backing_store_out->user_data = user_data;
    backing_store_out->vulkan.user_data = user_data;
    backing_store_out->vulkan.destruction_callback = [](void* user_data) {
      UserData* d = reinterpret_cast<UserData*>(user_data);
      d->surface->unref();
      delete d->image;
      delete d;
    };

    // The balancing unref is in the destruction callback.
    surface->ref();
  }

  return true;
#endif
  return false;
}

bool WaylandVulkanBackend::PresentLayers(const FlutterLayer** layers,
                                         size_t layers_count,
                                         void* user_data) {
  FML_DLOG(INFO) << "PresentLayers";
  return false;
}
