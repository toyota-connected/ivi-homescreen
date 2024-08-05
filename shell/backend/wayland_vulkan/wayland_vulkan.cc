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
#include <optional>
#include <queue>

#include "config/common.h"
#include "engine.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

const auto& d = vk::defaultDispatchLoaderDynamic;

#define S1(x) #x
#define S2(x) S1(x)
#define LOCATION __FILE__ " : " S2(__LINE__)

#define CHECK_VK_RESULT(x)                                 \
  do {                                                     \
    vk::detail::resultCheck(static_cast<vk::Result>(x), LOCATION); \
  } while (0)

WaylandVulkanBackend::WaylandVulkanBackend(struct wl_display* display,
                                           uint32_t width,
                                           uint32_t height,
                                           bool enable_validation_layers)
    : wl_display_(display),
      width_(width),
      height_(height),
      enable_validation_layers_(enable_validation_layers),
      resize_pending_(false),
      Backend() {
  VULKAN_HPP_DEFAULT_DISPATCHER.init();
  createInstance();
  setupDebugMessenger();
}

FlutterRendererConfig WaylandVulkanBackend::GetRenderConfig() {
  return {
      .type = kVulkan,
      .vulkan{
          .struct_size = sizeof(FlutterRendererConfig),
          .version = VK_MAKE_VERSION(1, 1, 0),
          .instance = instance_,
          .physical_device = physical_device_,
          .device = device_,
          .queue_family_index = queue_family_index_,
          .queue = queue_,
          .enabled_instance_extension_count =
              enabled_instance_extensions_.size(),
          .enabled_instance_extensions = enabled_instance_extensions_.data(),
          .enabled_device_extension_count = enabled_device_extensions_.size(),
          .enabled_device_extensions = enabled_device_extensions_.data(),
          .get_instance_proc_address_callback = GetInstanceProcAddressCallback,
          .get_next_image_callback = GetNextImageCallback,
          .present_image_callback = PresentCallback,
      }};
}

FlutterCompositor WaylandVulkanBackend::GetCompositorConfig() {
  return {.struct_size = sizeof(FlutterCompositor),
          .user_data = this,
          .create_backing_store_callback = nullptr,   // CreateBackingStore,
          .collect_backing_store_callback = nullptr,  // CollectBackingStore,
          .present_layers_callback = nullptr,         // PresentLayers,
          .avoid_backing_store_cache = true};
}

WaylandVulkanBackend::~WaylandVulkanBackend() {
  if (device_ != nullptr) {
    if (swapchain_command_pool_ != nullptr) {
      d.vkDestroyCommandPool(device_, swapchain_command_pool_, nullptr);
    }
    if (present_transition_semaphore_ != nullptr) {
      d.vkDestroySemaphore(device_, present_transition_semaphore_, nullptr);
    }
    if (image_ready_fence_ != nullptr) {
      d.vkDestroyFence(device_, image_ready_fence_, nullptr);
    }
    d.vkDestroyDevice(device_, nullptr);
  }
  if (surface_ != nullptr) {
    d.vkDestroySurfaceKHR(instance_, surface_, nullptr);
  }
  if (enable_validation_layers_) {
    if (mDebugCallback) {
      d.vkDestroyDebugReportCallbackEXT(instance_, mDebugCallback, VKALLOC);
    }
    if (mDebugMessenger) {
      d.vkDestroyDebugUtilsMessengerEXT(instance_, mDebugMessenger, VKALLOC);
    }
  }
  if (instance_ != nullptr) {
    d.vkDestroyInstance(instance_, nullptr);
  }
  if (!enabled_instance_extensions_.empty()) {
    for (auto it : enabled_instance_extensions_) {
      free((void*)it);
    }
  }
  if (!enabled_layer_extensions_.empty()) {
    for (auto it : enabled_layer_extensions_) {
      free((void*)it);
    }
  }
}

void WaylandVulkanBackend::createInstance() {
  auto instance_extensions = vk::enumerateInstanceExtensionProperties();
  spdlog::debug("Vulkan Instance Extensions:");
  for (const auto& l : instance_extensions.value) {
    spdlog::debug("\t{}, version: {}", l.extensionName.data(), l.specVersion);
    if (enable_validation_layers_) {
      if (strcmp(l.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) ==
          0) {
        enabled_instance_extensions_.push_back(strdup(l.extensionName));
      }
      if (strcmp(l.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
        debugUtilsSupported_ = true;
        enabled_instance_extensions_.push_back(strdup(l.extensionName));
      }
      if (strcmp(l.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) {
        enabled_instance_extensions_.push_back(strdup(l.extensionName));
      }
    }
    if (strcmp(l.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
      surfaceSupported_ = true;
      enabled_instance_extensions_.push_back(strdup(l.extensionName));
    }
    if (strcmp(l.extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0) {
      waylandSurfaceSupported_ = true;
      enabled_instance_extensions_.push_back(strdup(l.extensionName));
    }
  }

  if (!surfaceSupported_ || !waylandSurfaceSupported_) {
    spdlog::critical(
        "This Vulkan driver does not support the minimum required extensions");
    exit(EXIT_FAILURE);
  }

  std::stringstream ss;
  ss << "Enabling " << enabled_instance_extensions_.size()
     << " instance extensions:";
  for (auto& extension : enabled_instance_extensions_) {
    ss << "\n\t" << extension;
  }
  spdlog::info(ss.str());

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = kApplicationName;
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "No Engine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.apiVersion = VK_MAKE_VERSION(1, 1, 0);

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app_info;
  info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_instance_extensions_.size());
  info.ppEnabledExtensionNames = enabled_instance_extensions_.data();

  static constexpr char VK_LAYER_KHRONOS_VALIDATION_NAME[] =
      "VK_LAYER_KHRONOS_validation";

  auto available_layers = vk::enumerateInstanceLayerProperties();
  spdlog::debug("Vulkan Instance Layers:");
  for (const auto& l : available_layers.value) {
    spdlog::debug("\t{} - {}", l.layerName.data(), l.description.data());
    if (enable_validation_layers_ &&
        strcmp(l.layerName, VK_LAYER_KHRONOS_VALIDATION_NAME) == 0) {
      enabled_layer_extensions_.push_back(VK_LAYER_KHRONOS_VALIDATION_NAME);
      break;
    }
  }

  ss.clear();
  ss.str("");
  ss << "Enabling " << enabled_layer_extensions_.size() << " layer extensions:";
  for (const auto& layer : enabled_layer_extensions_) {
    ss << "\n\t" << layer;
  }
  spdlog::info(ss.str());

  info.enabledLayerCount =
      static_cast<uint32_t>(enabled_layer_extensions_.size());
  info.ppEnabledLayerNames = enabled_layer_extensions_.data();

  CHECK_VK_RESULT(d.vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS);

  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
}

void WaylandVulkanBackend::setupDebugMessenger() {
  if (!enable_validation_layers_)
    return;

  if (debugUtilsSupported_) {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugUtilsCallback;

    CHECK_VK_RESULT(d.vkCreateDebugUtilsMessengerEXT(
                        instance_, &createInfo, VKALLOC, &mDebugMessenger) !=
                    VK_SUCCESS);
  } else if (d.vkCreateDebugReportCallbackEXT) {
    VkDebugReportCallbackCreateInfoEXT cb_info{};
    cb_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    cb_info.flags =
        VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT;
    cb_info.pfnCallback = debugReportCallback;
    CHECK_VK_RESULT(d.vkCreateDebugReportCallbackEXT(
                        instance_, &cb_info, VKALLOC, &mDebugCallback) !=
                    VK_SUCCESS);
  }
}

void WaylandVulkanBackend::findPhysicalDevice() {
  uint32_t count;
  CHECK_VK_RESULT(d.vkEnumeratePhysicalDevices(instance_, &count, nullptr));
  std::vector<VkPhysicalDevice> physical_devices(count);
  CHECK_VK_RESULT(
      d.vkEnumeratePhysicalDevices(instance_, &count, physical_devices.data()));

  SPDLOG_DEBUG("Enumerating {} physical device(s).", count);

  uint32_t selected_score = 0;
  for (const auto& physical_device : physical_devices) {
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    d.vkGetPhysicalDeviceProperties(physical_device, &properties);
    d.vkGetPhysicalDeviceFeatures(physical_device, &features);

    SPDLOG_DEBUG("Checking device: {}", properties.deviceName);

    uint32_t score = 0;
    std::vector<const char*> supported_extensions;

    uint32_t qfp_count;
    d.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qfp_count,
                                               nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qfp_count);
    d.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qfp_count,
                                               qfp.data());
    std::optional<uint32_t> graphics_queue_family;
    for (uint32_t i = 0; i < qfp.size(); i++) {
      // Only pick graphics queues that can also present to the surface.
      // Graphics queues that can't present are rare if not nonexistent, but
      // the spec allows for this, so check it anyhow.
      VkBool32 surface_present_supported;
      CHECK_VK_RESULT(d.vkGetPhysicalDeviceSurfaceSupportKHR(
          physical_device, i, surface_, &surface_present_supported));

      if (!graphics_queue_family.has_value() &&
          qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
          surface_present_supported) {
        graphics_queue_family = i;
      }
    }

    // Skip physical devices that don't have a graphics queue.
    if (!graphics_queue_family.has_value()) {
      spdlog::info("  - Skipping due to no suitable graphics queues.");
      continue;
    }

    // Prefer discrete GPUs.
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score += 1 << 30;
    }

    uint32_t extension_count;
    CHECK_VK_RESULT(d.vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &extension_count, nullptr));
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    CHECK_VK_RESULT(d.vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &extension_count,
        available_extensions.data()));

    bool supports_swapchain = false;
    for (const auto& available_extension : available_extensions) {
      if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                 available_extension.extensionName) == 0) {
        supports_swapchain = true;
        supported_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      }
      // The spec requires VK_KHR_portability_subset be enabled whenever it's
      // available on a device. It's present on compatibility ICDs like
      // MoltenVK.
      else if (strcmp("VK_KHR_portability_subset",
                      available_extension.extensionName) == 0) {
        supported_extensions.push_back("VK_KHR_portability_subset");
      }
      // Prefer GPUs that support VK_KHR_get_memory_requirements2.
      else if (strcmp(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                      available_extension.extensionName) == 0) {
        score += 1 << 29;
        supported_extensions.push_back(
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
      }
    }

    // Skip physical devices that don't have swapchain support.
    if (!supports_swapchain) {
      SPDLOG_DEBUG("  - Skipping due to lack of swapchain support.");
      continue;
    }

    // Prefer GPUs with larger max texture sizes.
    score += properties.limits.maxImageDimension2D;

    if (selected_score < score) {
      SPDLOG_DEBUG("  - This is the best device so far. Score: 0x{:x}", score);

      selected_score = score;
      physical_device_ = physical_device;
      enabled_device_extensions_ = supported_extensions;
      queue_family_index_ =
          graphics_queue_family.value_or(std::numeric_limits<uint32_t>::max());

      // Bingo, we finally found a physical device that supports everything we
      // need.
      d.vkGetPhysicalDeviceFeatures(physical_device,
                                    &physical_device_features_);
      d.vkGetPhysicalDeviceMemoryProperties(
          physical_device, &physical_device_memory_properties_);

      // Print some driver or MoltenVK information if it is available.
      if (d.vkGetPhysicalDeviceProperties2KHR) {
        VkPhysicalDeviceDriverProperties driverProperties{};
        driverProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDeviceProperties2 physicalDeviceProperties2{};
        physicalDeviceProperties2.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        physicalDeviceProperties2.pNext = &driverProperties;

        d.vkGetPhysicalDeviceProperties2KHR(physical_device_,
                                            &physicalDeviceProperties2);
        spdlog::info("Vulkan device driver: {} {}", driverProperties.driverName,
                     driverProperties.driverInfo);
      }

      // Print out some properties of the GPU for diagnostic purposes.
      spdlog::info("vendor {:x}, device {:x}, driver {:x}, api {}.{}",
                   properties.vendorID, properties.deviceID,
                   properties.driverVersion,
                   VK_VERSION_MAJOR(properties.apiVersion),
                   VK_VERSION_MINOR(properties.apiVersion));
      break;
    }
  }

  if (physical_device_ == nullptr) {
    spdlog::critical("Failed to find a compatible Vulkan physical device.");
    exit(EXIT_FAILURE);
  }
}

void WaylandVulkanBackend::createLogicalDevice() {
#if !defined(NDEBUG)
  std::stringstream ss;
  ss << "Enabling " << enabled_device_extensions_.size()
     << " device extensions:";
  for (const char* extension : enabled_device_extensions_) {
    ss << "  - " << extension;
  }
  SPDLOG_DEBUG(ss.str());
#endif

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = queue_family_index_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures device_features{};
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();
  device_info.pEnabledFeatures = &device_features;

  CHECK_VK_RESULT(
      d.vkCreateDevice(physical_device_, &device_info, nullptr, &device_));

  d.vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
}

bool WaylandVulkanBackend::InitializeSwapchain() {
  if (resize_pending_) {
    resize_pending_ = false;
    d.vkDestroySwapchainKHR(device_, swapchain_, nullptr);

    CHECK_VK_RESULT(d.vkQueueWaitIdle(queue_));
    CHECK_VK_RESULT(
        d.vkResetCommandPool(device_, swapchain_command_pool_,
                             VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
  }

  // --------------------------------------------------------------------------
  // Choose an image format that can be presented to the surface, preferring
  // the common BGRA+sRGB if available.
  // --------------------------------------------------------------------------

  uint32_t format_count;
  CHECK_VK_RESULT(d.vkGetPhysicalDeviceSurfaceFormatsKHR(
      physical_device_, surface_, &format_count, nullptr));
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  CHECK_VK_RESULT(d.vkGetPhysicalDeviceSurfaceFormatsKHR(
      physical_device_, surface_, &format_count, formats.data()));

  surface_format_ = formats[0];
  for (const auto& format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      surface_format_ = format;
      break;
    }
  }

  // --------------------------------------------------------------------------
  // Choose the presentable image size that's as close as possible to the
  // window size.
  // --------------------------------------------------------------------------

  VkExtent2D clientSize;

  VkSurfaceCapabilitiesKHR surface_capabilities;
  CHECK_VK_RESULT(d.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      physical_device_, surface_, &surface_capabilities));

  if (surface_capabilities.currentExtent.width != UINT32_MAX) {
    // If the surface reports a specific extent, we must use it.
    clientSize = surface_capabilities.currentExtent;
  } else {
    VkExtent2D actual_extent{};
    actual_extent.width = width_;
    actual_extent.height = height_;

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
    spdlog::error("Swap chain does not support {} images.", desiredImageCount);
    desiredImageCount = surface_capabilities.minImageCount;
  }

  // --------------------------------------------------------------------------
  // Choose the present mode.
  // --------------------------------------------------------------------------

  uint32_t mode_count;
  CHECK_VK_RESULT(d.vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device_, surface_, &mode_count, nullptr));
  std::vector<VkPresentModeKHR> modes(mode_count);
  CHECK_VK_RESULT(d.vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device_, surface_, &mode_count, modes.data()));
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

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = surface_;
  info.minImageCount = desiredImageCount;
  info.imageFormat = surface_format_.format;
  info.imageColorSpace = surface_format_.colorSpace;
  info.imageExtent = clientSize;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = surface_capabilities.currentTransform;
  info.compositeAlpha = compositeAlpha;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;

  auto result = d.vkCreateSwapchainKHR(device_, &info, VKALLOC, &swapchain_);
  CHECK_VK_RESULT(result);
  if (result != VK_SUCCESS) {
    return false;
  }

  // --------------------------------------------------------------------------
  // Fetch swapchain images
  // --------------------------------------------------------------------------

  uint32_t image_count;
  CHECK_VK_RESULT(
      d.vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr));
  swapchain_images_.reserve(image_count);
  swapchain_images_.resize(image_count);
  CHECK_VK_RESULT(d.vkGetSwapchainImagesKHR(device_, swapchain_, &image_count,
                                            swapchain_images_.data()));

  // --------------------------------------------------------------------------
  // Record a command buffer for each of the images to be executed prior to
  // presenting.
  // --------------------------------------------------------------------------

  present_transition_buffers_.resize(swapchain_images_.size());

  VkCommandBufferAllocateInfo buffers_info{};
  buffers_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  buffers_info.commandPool = swapchain_command_pool_;
  buffers_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  buffers_info.commandBufferCount =
      static_cast<uint32_t>(present_transition_buffers_.size());

  CHECK_VK_RESULT(d.vkAllocateCommandBuffers(
      device_, &buffers_info, present_transition_buffers_.data()));

  for (size_t i = 0; i < swapchain_images_.size(); i++) {
    auto image = swapchain_images_[i];
    auto buffer = present_transition_buffers_[i];

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CHECK_VK_RESULT(d.vkBeginCommandBuffer(buffer, &begin_info));

    // Filament Engine hands back the image after writing to it
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    d.vkCmdPipelineBarrier(buffer,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);

    CHECK_VK_RESULT(d.vkEndCommandBuffer(buffer));
  }

  return true;
}

VKAPI_ATTR VkBool32

    VKAPI_CALL
    WaylandVulkanBackend::debugReportCallback(
        VkDebugReportFlagsEXT flags,
        VkDebugReportObjectTypeEXT objectType,
        uint64_t object,
        size_t location,
        int32_t messageCode,
        const char* pLayerPrefix,
        const char* pMessage,
        void* pUserData) {
  (void)objectType;
  (void)object;
  (void)location;
  (void)messageCode;
  (void)pUserData;
  if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
    spdlog::error("VULKAN ERROR: ({}) {}", pLayerPrefix, pMessage);
  } else {
    spdlog::warn("VULKAN WARNING: ({}) {}", pLayerPrefix, pMessage);
  }
  return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL WaylandVulkanBackend::debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* cbdata,
    void* pUserData) {
  (void)types;
  (void)pUserData;
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    spdlog::error("VULKAN ERROR: ({}) {}", cbdata->pMessageIdName,
                  cbdata->pMessage);
  } else {
    // TODO: emit best practices warnings about aggressive pipeline barriers.
    if (strstr(cbdata->pMessage, "ALL_GRAPHICS_BIT") ||
        strstr(cbdata->pMessage, "ALL_COMMANDS_BIT")) {
      return VK_FALSE;
    }
    spdlog::warn("VULKAN WARNING: ({}) {}", cbdata->pMessageIdName,
                 cbdata->pMessage);
  }
  return VK_TRUE;
}

FlutterVulkanImage WaylandVulkanBackend::GetNextImageCallback(
    void* user_data,
    const FlutterFrameInfo* frame_info) {
  (void)frame_info;
  if (frame_info->struct_size != sizeof(FlutterFrameInfo)) {
    SPDLOG_ERROR(
        "GetNextImageCallback: frame_info->struct_size != "
        "sizeof(FlutterFrameInfo)");
  }
  // If the framebuffer has been resized, discard the swapchain and create
  // a new one.

  auto state = reinterpret_cast<FlutterDesktopEngineState*>(user_data);
  auto b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  if (b->resize_pending_) {
    b->InitializeSwapchain();
  }

  CHECK_VK_RESULT(d.vkAcquireNextImageKHR(b->device_, b->swapchain_, UINT64_MAX,
                                          nullptr, b->image_ready_fence_,
                                          &b->last_image_index_));

  // Flutter Engine expects the image to be available for transitioning and
  // attaching immediately, and so we need to force a host sync here before
  // returning.
  CHECK_VK_RESULT(d.vkWaitForFences(b->device_, 1, &b->image_ready_fence_, true,
                                    UINT64_MAX));
  CHECK_VK_RESULT(d.vkResetFences(b->device_, 1, &b->image_ready_fence_));

  return {
      .struct_size = sizeof(FlutterVulkanImage),
      .image = reinterpret_cast<uint64_t>(
          b->swapchain_images_[b->last_image_index_]),
      .format = b->surface_format_.format,
  };
}

bool WaylandVulkanBackend::PresentCallback(void* user_data,
                                           const FlutterVulkanImage* image) {
  (void)image;
  auto state = reinterpret_cast<FlutterDesktopEngineState*>(user_data);
  auto b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  VkPipelineStageFlags stage_flags =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pWaitDstStageMask = &stage_flags;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers =
      &b->present_transition_buffers_[b->last_image_index_];
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &b->present_transition_semaphore_;
  d.vkQueueSubmit(b->queue_, 1, &submit_info, nullptr);

  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &b->present_transition_semaphore_;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &b->swapchain_;
  present_info.pImageIndices = &b->last_image_index_;
  VkResult result = d.vkQueuePresentKHR(b->queue_, &present_info);

  // If the swapchain is no longer compatible with the surface, discard the
  // swapchain and create a new one.
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    b->InitializeSwapchain();
  }
  d.vkQueueWaitIdle(b->queue_);

  return result == VK_SUCCESS;
}

void* WaylandVulkanBackend::GetInstanceProcAddressCallback(
    void* user_data,
    FlutterVulkanInstanceHandle instance,
    const char* procname) {
  (void)user_data;
  auto* proc =
      d.vkGetInstanceProcAddr(reinterpret_cast<VkInstance>(instance), procname);
  return reinterpret_cast<void*>(proc);
}

void WaylandVulkanBackend::Resize(size_t index,
                                  Engine* flutter_engine,
                                  int32_t width,
                                  int32_t height) {
  (void)index;
  if (width_ != width || height_ != height) {
    resize_pending_ = true;
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
    if (flutter_engine) {
      auto result = flutter_engine->SetWindowSize(static_cast<size_t>(height),
                                                  static_cast<size_t>(width));
      if (result != kSuccess) {
        spdlog::error("Failed to set Flutter Engine Window Size");
      }
    }
  }
}

void WaylandVulkanBackend::CreateSurface(size_t index,
                                         struct wl_surface* surface,
                                         int32_t width,
                                         int32_t height) {
  (void)index;
  (void)width;
  (void)height;

  SPDLOG_DEBUG("CreateSurface");
  assert(instance_ != VK_NULL_HANDLE);
  assert(surface_ == VK_NULL_HANDLE);
  assert(wl_display_ != nullptr);
  assert(surface != nullptr);

  surface_ = VK_NULL_HANDLE;

  VkWaylandSurfaceCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  createInfo.display = wl_display_;
  createInfo.surface = surface;

  CHECK_VK_RESULT(
      d.vkCreateWaylandSurfaceKHR(instance_, &createInfo, nullptr, &surface_));

  findPhysicalDevice();
  createLogicalDevice();

  // --------------------------------------------------------------------------
  // Create sync primitives and command pool to use in the render loop
  // callbacks.
  // --------------------------------------------------------------------------

  VkFenceCreateInfo f_info{};
  f_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  d.vkCreateFence(device_, &f_info, nullptr, &image_ready_fence_);

  VkSemaphoreCreateInfo s_info{};
  s_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  d.vkCreateSemaphore(device_, &s_info, nullptr,
                      &present_transition_semaphore_);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = queue_family_index_;
  d.vkCreateCommandPool(device_, &pool_info, nullptr, &swapchain_command_pool_);

  if (!InitializeSwapchain()) {
    spdlog::critical("Failed to create swapchain.");
    exit(EXIT_FAILURE);
  }
}

bool WaylandVulkanBackend::CollectBackingStore(
    const FlutterBackingStore* renderer,
    void* user_data) {
  (void)renderer;
  (void)user_data;
  SPDLOG_DEBUG("CollectBackingStore");
  return false;
}

bool WaylandVulkanBackend::CreateBackingStore(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out,
    void* user_data) {
  (void)config;
  (void)backing_store_out;
  (void)user_data;
  SPDLOG_DEBUG("CreateBackingStore");
#if 0
    auto surface_size = SkISize::Make(config->size.width, config->size.height);
    TestVulkanImage* test_image = new TestVulkanImage(
        std::move(test_vulkan_context_->CreateImage(surface_size).value()));

    GrVkImageInfo image_info{};
    image_info.fImage = test_image->GetImage();
    image_info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.fFormat = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.fImageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.fSampleCount = 1;
    image_info.fLevelCount = 1;

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
      spdlog::error("Could not create Skia surface from Vulkan image.");
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
  (void)layers;
  (void)layers_count;
  (void)user_data;
  SPDLOG_DEBUG("PresentLayers");
  return false;
}

bool WaylandVulkanBackend::TextureMakeCurrent() {
  return true;
}

bool WaylandVulkanBackend::TextureClearCurrent() {
  return true;
}
