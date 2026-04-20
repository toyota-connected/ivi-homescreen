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
#include "logging.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

const auto& d = vk::detail::defaultDispatchLoaderDynamic;

#define S1(x) #x
#define S2(x) S1(x)
#define LOCATION __FILE__ " : " S2(__LINE__)

#define CHECK_VK_RESULT(x)                                         \
  do {                                                             \
    vk::detail::resultCheck(static_cast<vk::Result>(x), LOCATION); \
  } while (0)

WaylandVulkanBackend::WaylandVulkanBackend(wl_display* display,
                                           const uint32_t width,
                                           const uint32_t height,
                                           const bool enable_validation_layers)
    : Backend(),
      enable_validation_layers_(enable_validation_layers),
      resize_pending_(false),
      wl_display_(display),
      width_(width),
      height_(height) {
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
#if BUILD_COMPOSITOR
  return {
      .struct_size = sizeof(FlutterCompositor),
      .user_data = this,
      .create_backing_store_callback = CreateBackingStore,
      .collect_backing_store_callback = CollectBackingStore,
      .present_layers_callback = PresentLayers,
      .avoid_backing_store_cache = false,
      .present_view_callback = nullptr,
  };
#else
  return {
      .struct_size = sizeof(FlutterCompositor),
      .user_data = this,
      .create_backing_store_callback = nullptr,
      .collect_backing_store_callback = nullptr,
      .present_layers_callback = nullptr,
      .avoid_backing_store_cache = true,
      .present_view_callback = nullptr,
  };
#endif
}

WaylandVulkanBackend::~WaylandVulkanBackend() {
  if (device_ != nullptr) {
#if BUILD_COMPOSITOR
    CompositorPipeliningCleanup();
#endif
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
    for (const auto it : enabled_instance_extensions_) {
      free((void*)it);
    }
  }
  if (!enabled_layer_extensions_.empty()) {
    for (const auto it : enabled_layer_extensions_) {
      free((void*)it);
    }
  }
}

void WaylandVulkanBackend::createInstance() {
  debugUtilsSupported_ = false;
  surfaceSupported_ = false;
  waylandSurfaceSupported_ = false;
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

  if (enable_validation_layers_) {
    constexpr char layer_name[] = "VK_LAYER_KHRONOS_validation";
    constexpr VkBool32 setting_validate_core = VK_TRUE;
    constexpr VkBool32 setting_validate_sync = VK_TRUE;
    constexpr VkBool32 setting_thread_safety = VK_TRUE;
    const char* setting_debug_action[] = {"VK_DBG_LAYER_ACTION_LOG_MSG"};
    const char* setting_report_flags[] = {"error", "warn", "info", "perf",
                                          "verbose"};
    constexpr VkBool32 setting_enable_message_limit = VK_TRUE;
    constexpr uint32_t setting_duplicate_message_limit = 3;

    const VkLayerSettingEXT settings[] = {
        {layer_name, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
         &setting_validate_core},
        {layer_name, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
         &setting_validate_sync},
        {layer_name, "thread_safety", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
         &setting_thread_safety},
        {layer_name, "debug_action", VK_LAYER_SETTING_TYPE_STRING_EXT, 1,
         setting_debug_action},
        {layer_name, "report_flags", VK_LAYER_SETTING_TYPE_STRING_EXT,
         static_cast<uint32_t>(std::size(setting_report_flags)),
         setting_report_flags},
        {layer_name, "enable_message_limit", VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         1, &setting_enable_message_limit},
        {layer_name, "duplicate_message_limit",
         VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &setting_duplicate_message_limit},
    };

    VkLayerSettingsCreateInfoEXT layer_settings_create_info{};
    layer_settings_create_info.sType =
        VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layer_settings_create_info.settingCount =
        static_cast<uint32_t>(std::size(settings));
    layer_settings_create_info.pSettings = settings;

    info.pNext = &layer_settings_create_info;
  }

  constexpr char VK_LAYER_KHRONOS_VALIDATION_NAME[] =
      "VK_LAYER_KHRONOS_validation";

  const auto available_layers = vk::enumerateInstanceLayerProperties();
  if (!available_layers.value.empty()) {
    spdlog::debug("Vulkan Instance Layers:");
    for (const auto& l : available_layers.value) {
      spdlog::debug("\t{} - {}", l.layerName.data(), l.description.data());
      if (enable_validation_layers_ &&
          strcmp(l.layerName, VK_LAYER_KHRONOS_VALIDATION_NAME) == 0) {
        enabled_layer_extensions_.push_back(VK_LAYER_KHRONOS_VALIDATION_NAME);
        break;
      }
    }
  }

  if (!enabled_layer_extensions_.empty()) {
    ss.clear();
    ss.str("");
    ss << "Enabling " << enabled_layer_extensions_.size()
       << " layer extensions:";
    for (const auto& layer : enabled_layer_extensions_) {
      ss << "\n\t" << layer;
    }
    spdlog::info(ss.str());
  }

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

    bool supports_swap_chain = false;
    for (const auto& [extensionName, specVersion] : available_extensions) {
      if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME, extensionName) == 0) {
        supports_swap_chain = true;
        supported_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      }
      // The spec requires VK_KHR_portability_subset be enabled whenever it's
      // available on a device. It's present on compatibility ICDs like
      // MoltenVK.
      else if (strcmp("VK_KHR_portability_subset", extensionName) == 0) {
        supported_extensions.push_back("VK_KHR_portability_subset");
      }
      // Prefer GPUs that support VK_KHR_get_memory_requirements2.
      else if (strcmp(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                      extensionName) == 0) {
        score += 1 << 29;
        supported_extensions.push_back(
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
      }
    }

    // Skip physical devices that don't have swap chain support.
    if (!supports_swap_chain) {
      SPDLOG_DEBUG("  - Skipping due to lack of swap chain support.");
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

bool WaylandVulkanBackend::InitializeSwapChain() {
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
  // Create the swap chain.
  // --------------------------------------------------------------------------
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
  info.compositeAlpha = (surface_capabilities.supportedCompositeAlpha &
                         VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                            : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;

  auto result = d.vkCreateSwapchainKHR(device_, &info, VKALLOC, &swapchain_);
  CHECK_VK_RESULT(result);
  if (result != VK_SUCCESS) {
    return false;
  }

  // --------------------------------------------------------------------------
  // Fetch swap chain images
  // --------------------------------------------------------------------------

  uint32_t image_count;
  CHECK_VK_RESULT(
      d.vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr));
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
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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

#if BUILD_COMPOSITOR
  CompositorPipeliningInit();
#endif

  return true;
}

VKAPI_ATTR VkBool32

    VKAPI_CALL
    WaylandVulkanBackend::debugReportCallback(
        const VkDebugReportFlagsEXT flags,
        const VkDebugReportObjectTypeEXT /* objectType */,
        const uint64_t /* object */,
        const size_t /* location */,
        const int32_t /* messageCode */,
        const char* pLayerPrefix,
        const char* pMessage,
        void* /* pUserData */) {
  if (flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) {
    spdlog::info("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) {
    spdlog::warn("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) {
    spdlog::warn("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
    spdlog::error("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT) {
    spdlog::debug("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  }
  return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL WaylandVulkanBackend::debugUtilsCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    const VkDebugUtilsMessageTypeFlagsEXT /* types */,
    const VkDebugUtilsMessengerCallbackDataEXT* cb_data,
    void* /* pUserData */) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
    spdlog::info("Vulkan Dbg: ({}) {}", cb_data->pMessageIdName,
                 cb_data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    spdlog::info("Vulkan Dbg: ({}) {}", cb_data->pMessageIdName,
                 cb_data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    spdlog::info("Vulkan Dbg: ({}) {}", cb_data->pMessageIdName,
                 cb_data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    spdlog::error("Vulkan Dbg: ({}) {}", cb_data->pMessageIdName,
                  cb_data->pMessage);
  }
  return VK_TRUE;
}

FlutterVulkanImage WaylandVulkanBackend::GetNextImageCallback(
    void* user_data,
    const FlutterFrameInfo* frame_info) {
  if (frame_info->struct_size != sizeof(FlutterFrameInfo)) {
    SPDLOG_ERROR(
        "GetNextImageCallback: frame_info->struct_size != "
        "sizeof(FlutterFrameInfo)");
  }
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  const auto b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  if (b->resize_pending_) {
    b->InitializeSwapChain();
  }

  CHECK_VK_RESULT(d.vkAcquireNextImageKHR(
      b->device_, b->swapchain_, 1'000'000'000,  // timeout (ns) 1000ms
      nullptr, b->image_ready_fence_, &b->last_image_index_));

  CHECK_VK_RESULT(d.vkWaitForFences(b->device_, 1, &b->image_ready_fence_, true,
                                    UINT64_MAX));
  CHECK_VK_RESULT(d.vkResetFences(b->device_, 1, &b->image_ready_fence_));

  return {
      .struct_size = sizeof(FlutterVulkanImage),
      .image = reinterpret_cast<uint64_t>(
          b->swapchain_images_[b->last_image_index_]),
      .format = static_cast<uint32_t>(b->surface_format_.format),
  };
}

bool WaylandVulkanBackend::PresentCallback(
    void* user_data,
    const FlutterVulkanImage* /* image */) {
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  const auto b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());

  // Ensure the layout transition happens after the render pass in the same
  // command buffer Record vkCmdPipelineBarrier at the end of your render pass
  // command buffer

  // Submit the command buffer and signal the semaphore
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers =
      &b->present_transition_buffers_[b->last_image_index_];
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &b->present_transition_semaphore_;
  d.vkQueueSubmit(b->queue_, 1, &submit_info, nullptr);

  // Wait on the signaled semaphore in vkQueuePresentKHR
  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &b->present_transition_semaphore_;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &b->swapchain_;
  present_info.pImageIndices = &b->last_image_index_;
  const VkResult result = d.vkQueuePresentKHR(b->queue_, &present_info);

  // If the swap chain is no longer compatible with the surface, discard the
  // swap chain and create a new one.
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    b->InitializeSwapChain();
  }
  d.vkQueueWaitIdle(b->queue_);

  return result == VK_SUCCESS;
}

void* WaylandVulkanBackend::GetInstanceProcAddressCallback(
    void* /* user_data */,
    FlutterVulkanInstanceHandle instance,
    const char* procname) {
  auto* proc =
      d.vkGetInstanceProcAddr(static_cast<VkInstance>(instance), procname);
  return reinterpret_cast<void*>(proc);
}

void WaylandVulkanBackend::Resize(size_t /* index */,
                                  Engine* engine,
                                  const int32_t width,
                                  const int32_t height) {
  if (width_ != static_cast<uint32_t>(width) ||
      height_ != static_cast<uint32_t>(height)) {
    resize_pending_ = true;
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
    if (engine) {
      if (engine->SetWindowSize(static_cast<size_t>(height),
                                static_cast<size_t>(width)) != kSuccess) {
        spdlog::error("Failed to set Flutter Engine Window Size");
      }
    }
  }
}

void WaylandVulkanBackend::CreateSurface(size_t /* index */,
                                         wl_surface* surface,
                                         int32_t /* width */,
                                         int32_t /* height */) {
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

  if (!InitializeSwapChain()) {
    spdlog::critical("Failed to create swap chain.");
    exit(EXIT_FAILURE);
  }
}

bool WaylandVulkanBackend::CollectBackingStore(const FlutterBackingStore* store,
                                               void* user_data) {
#if BUILD_COMPOSITOR
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  auto* b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  return b->CollectBackingStoreImpl(store);
#else
  (void)store;
  (void)user_data;
  SPDLOG_DEBUG("CollectBackingStore");
  return false;
#endif
}

bool WaylandVulkanBackend::CreateBackingStore(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out,
    void* user_data) {
#if BUILD_COMPOSITOR
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  auto* b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  return b->CreateBackingStoreImpl(config, backing_store_out);
#else
  (void)config;
  (void)backing_store_out;
  (void)user_data;
  SPDLOG_DEBUG("CreateBackingStore");
#if 0  /// TODO
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
#endif  // BUILD_COMPOSITOR
}

bool WaylandVulkanBackend::PresentLayers(const FlutterLayer** layers,
                                         size_t layers_count,
                                         void* user_data) {
#if BUILD_COMPOSITOR
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  auto* b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  return b->PresentLayersImpl(layers, layers_count);
#else
  (void)layers;
  (void)layers_count;
  (void)user_data;
  SPDLOG_DEBUG("PresentLayers");
  return false;
#endif
}

bool WaylandVulkanBackend::TextureMakeCurrent() {
  return true;
}

bool WaylandVulkanBackend::TextureClearCurrent() {
  return true;
}

#if BUILD_COMPOSITOR

namespace {
struct VulkanStoreBaton {
  WaylandVulkanBackend* backend;
  VulkanBackingStore* store;
};
}  // namespace

void WaylandVulkanBackend::RegisterCompositorSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  m_compositor_surfaces[id] = std::move(surface);
}

void WaylandVulkanBackend::UnregisterCompositorSurface(
    FlutterPlatformViewIdentifier id) {
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  m_compositor_surfaces.erase(id);
}

void WaylandVulkanBackend::ResizeCompositorSurface(
    FlutterPlatformViewIdentifier id,
    int32_t width,
    int32_t height) {
  std::shared_ptr<ICompositorSurface> surface;
  {
    std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
    const auto it = m_compositor_surfaces.find(id);
    if (it == m_compositor_surfaces.end()) {
      return;
    }
    surface = it->second;
  }
  if (surface) {
    surface->OnResize(width, height);
  }
}

bool WaylandVulkanBackend::CreateBackingStoreImpl(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* store_out) {
  const int32_t w = static_cast<int32_t>(config->size.width);
  const int32_t h = static_cast<int32_t>(config->size.height);

  const bool want_dma_buf = BUILD_COMPOSITOR_DMABUF_EXPORT != 0;
  auto store =
      m_store_pool.Acquire(w, h, device_, physical_device_, want_dma_buf);
  if (!store->IsValid()) {
    spdlog::error("WaylandVulkanBackend: failed to create backing store");
    return false;
  }
  // Record whether export succeeded at least once (for introspection).
  if (store->has_dma_buf()) {
    dma_buf_export_ok_ = true;
  }

  // Engine renders with the image in COLOR_ATTACHMENT_OPTIMAL; transition
  // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL before handing it over.
  if (store->Layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
    TransitionLayout(store->Image(), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    store->SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }

  auto* baton = new VulkanStoreBaton{this, store.get()};

  store_out->struct_size = sizeof(FlutterBackingStore);
  store_out->type = kFlutterBackingStoreTypeVulkan;
  store_out->user_data = baton;
  store_out->vulkan.struct_size = sizeof(FlutterVulkanBackingStore);
  store_out->vulkan.image = store->engine_image();
  store_out->vulkan.user_data = baton;
  store_out->vulkan.destruction_callback = [](void*) {
    // Store ownership lives in m_alive_stores; Collect does the cleanup.
  };

  VulkanBackingStore* key = store.get();
  m_alive_stores[key] = std::move(store);
  return true;
}

bool WaylandVulkanBackend::CollectBackingStoreImpl(
    const FlutterBackingStore* store) {
  auto* baton = static_cast<VulkanStoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
  const auto it = m_alive_stores.find(baton->store);
  if (it != m_alive_stores.end()) {
    m_store_pool.Release(std::move(it->second));
    m_alive_stores.erase(it);
  }
  delete baton;
  return true;
}

void WaylandVulkanBackend::TransitionLayout(
    VkImage image,
    VkImageLayout from,
    VkImageLayout to,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) const {
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = swapchain_command_pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  if (d.vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) {
    spdlog::error("TransitionLayout: vkAllocateCommandBuffers failed");
    return;
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d.vkBeginCommandBuffer(cmd, &begin);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT;

  d.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
  d.vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  d.vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
  d.vkQueueWaitIdle(queue_);
  d.vkFreeCommandBuffers(device_, swapchain_command_pool_, 1, &cmd);
}

void WaylandVulkanBackend::BlitStoreToSwapchain(VkCommandBuffer cmd,
                                                VulkanBackingStore& src,
                                                VkImage dst,
                                                int32_t dst_x,
                                                int32_t dst_y,
                                                int32_t dst_w,
                                                int32_t dst_h) const {
  // Transition source -> TRANSFER_SRC_OPTIMAL.
  VkImageMemoryBarrier src_to_xfer{};
  src_to_xfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  src_to_xfer.oldLayout = src.Layout();
  src_to_xfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  src_to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  src_to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  src_to_xfer.image = src.Image();
  src_to_xfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  src_to_xfer.subresourceRange.levelCount = 1;
  src_to_xfer.subresourceRange.layerCount = 1;
  src_to_xfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  src_to_xfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  d.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &src_to_xfer);

  VkImageBlit region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {0, 0, 0};
  region.srcOffsets[1] = {src.Width(), src.Height(), 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstOffsets[0] = {dst_x, dst_y, 0};
  region.dstOffsets[1] = {dst_x + dst_w, dst_y + dst_h, 1};
  d.vkCmdBlitImage(cmd, src.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);

  // Transition source back to COLOR_ATTACHMENT_OPTIMAL for the next frame.
  VkImageMemoryBarrier xfer_to_color = src_to_xfer;
  xfer_to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  xfer_to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  xfer_to_color.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  xfer_to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  d.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &xfer_to_color);
  src.SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

bool WaylandVulkanBackend::PresentLayersImpl(const FlutterLayer** layers,
                                             size_t count) {
  if (resize_pending_) {
    InitializeSwapChain();
  }
  if (m_compositor_slots_.empty()) {
    CompositorPipeliningInit();
  }

  // Pick the slot for this frame and wait until its previous use has
  // completed on the GPU. This is the only CPU/GPU sync — the queue is
  // never asked to idle.
  const FrameSlot& slot = m_compositor_slots_[m_compositor_current_frame_ %
                                              m_compositor_slots_.size()];
  CHECK_VK_RESULT(
      d.vkWaitForFences(device_, 1, &slot.in_flight, VK_TRUE, UINT64_MAX));
  CHECK_VK_RESULT(d.vkResetFences(device_, 1, &slot.in_flight));

  uint32_t image_index = 0;
  CHECK_VK_RESULT(d.vkAcquireNextImageKHR(device_, swapchain_, 1'000'000'000,
                                          slot.image_available, VK_NULL_HANDLE,
                                          &image_index));
  last_image_index_ = image_index;

  VkImage dst = swapchain_images_[image_index];

  VkCommandBuffer cmd = slot.cmd_buffer;
  d.vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d.vkBeginCommandBuffer(cmd, &begin);

  // Transition swapchain image UNDEFINED/PRESENT_SRC -> TRANSFER_DST_OPTIMAL.
  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = dst;
  to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  to_dst.subresourceRange.levelCount = 1;
  to_dst.subresourceRange.layerCount = 1;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  d.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

  // Sequence platform-view subsurface Z-order for this frame.
  m_sequencer.Present(
      layers, count, nullptr, [this](FlutterPlatformViewIdentifier id) {
        // PVs may choose the wl_subsurface route (sequencer)
        // or the ICompositorSurface texture route (below).
        // Only warn when *neither* is registered.
        std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
        if (m_compositor_surfaces.find(id) == m_compositor_surfaces.end()) {
          spdlog::warn(
              "Vulkan compositor: platform view {} has no "
              "registered subsurface or compositor surface",
              id);
        }
      });

  bool ok = true;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      auto* baton =
          static_cast<VulkanStoreBaton*>(layer->backing_store->user_data);
      if (!baton || !baton->store) {
        continue;
      }
      const auto dx = static_cast<int32_t>(layer->offset.x);
      const auto dy = static_cast<int32_t>(layer->offset.y);
      const auto dw = static_cast<int32_t>(layer->size.width);
      const auto dh = static_cast<int32_t>(layer->size.height);
      BlitStoreToSwapchain(cmd, *baton->store, dst, dx, dy, dw, dh);
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      const auto composed = MutationStack::Compose(layer->platform_view);
      if (composed.NeedsPluginComposite()) {
        spdlog::debug(
            "Vulkan compositor: platform view {} has non-trivial mutations "
            "(opacity={:.3f} rounded={} perspective={} axis_aligned={}); "
            "plugin OnPresent must apply them.",
            layer->platform_view->identifier, composed.opacity,
            composed.has_rounded_clip, composed.has_perspective,
            composed.IsAxisAligned());
      }
      std::shared_ptr<ICompositorSurface> surface;
      {
        std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
        const auto it =
            m_compositor_surfaces.find(layer->platform_view->identifier);
        if (it != m_compositor_surfaces.end()) {
          surface = it->second;
        }
      }
      if (surface) {
        ok = surface->OnPresent(layer) && ok;
      }
    }
  }

  // Transition swapchain image -> PRESENT_SRC_KHR.
  VkImageMemoryBarrier to_present{};
  to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_present.image = dst;
  to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  to_present.subresourceRange.levelCount = 1;
  to_present.subresourceRange.layerCount = 1;
  to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  d.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_present);

  d.vkEndCommandBuffer(cmd);

  // Submit waits on image_available (signaled when the swapchain image is
  // presentable), signals render_finished and the slot's in_flight fence.
  // No vkQueueWaitIdle — the next frame's slot wait is the only sync point.
  const VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &slot.image_available;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &slot.render_finished;
  d.vkQueueSubmit(queue_, 1, &submit, slot.in_flight);

  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &slot.render_finished;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &swapchain_;
  present_info.pImageIndices = &image_index;
  const VkResult result = d.vkQueuePresentKHR(queue_, &present_info);
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    resize_pending_ = true;
  }

  ++m_compositor_current_frame_;
  return ok;
}

void WaylandVulkanBackend::CompositorPipeliningInit() {
  CompositorPipeliningCleanup();

  if (swapchain_images_.empty()) {
    return;
  }

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index_;
  if (d.vkCreateCommandPool(device_, &pool_info, nullptr,
                            &m_compositor_cmd_pool_) != VK_SUCCESS) {
    spdlog::error("Vulkan compositor: failed to create cmd pool");
    return;
  }

  const auto slot_count = swapchain_images_.size();
  m_compositor_slots_.resize(slot_count);

  VkCommandBufferAllocateInfo cb_alloc{};
  cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_alloc.commandPool = m_compositor_cmd_pool_;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = static_cast<uint32_t>(slot_count);
  std::vector<VkCommandBuffer> cmds(slot_count);
  CHECK_VK_RESULT(d.vkAllocateCommandBuffers(device_, &cb_alloc, cmds.data()));

  // Fences start signaled so the first wait in PresentLayersImpl returns
  // immediately for every slot.
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  for (size_t i = 0; i < slot_count; ++i) {
    auto& s = m_compositor_slots_[i];
    s.cmd_buffer = cmds[i];
    CHECK_VK_RESULT(
        d.vkCreateFence(device_, &fence_info, nullptr, &s.in_flight));
    CHECK_VK_RESULT(
        d.vkCreateSemaphore(device_, &sem_info, nullptr, &s.image_available));
    CHECK_VK_RESULT(
        d.vkCreateSemaphore(device_, &sem_info, nullptr, &s.render_finished));
  }
  m_compositor_current_frame_ = 0;
}

void WaylandVulkanBackend::CompositorPipeliningCleanup() {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  // Make sure no slot's resources are still in flight before tearing them
  // down. Cheap: only fires at swapchain recreation or backend shutdown.
  if (!m_compositor_slots_.empty()) {
    d.vkDeviceWaitIdle(device_);
  }
  for (auto& s : m_compositor_slots_) {
    if (s.image_available) {
      d.vkDestroySemaphore(device_, s.image_available, nullptr);
    }
    if (s.render_finished) {
      d.vkDestroySemaphore(device_, s.render_finished, nullptr);
    }
    if (s.in_flight) {
      d.vkDestroyFence(device_, s.in_flight, nullptr);
    }
    s = {};
  }
  m_compositor_slots_.clear();
  if (m_compositor_cmd_pool_ != VK_NULL_HANDLE) {
    d.vkDestroyCommandPool(device_, m_compositor_cmd_pool_, nullptr);
    m_compositor_cmd_pool_ = VK_NULL_HANDLE;
  }
  m_compositor_current_frame_ = 0;
}

#endif  // BUILD_COMPOSITOR
