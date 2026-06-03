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

// vulkan.hpp's dynamic dispatcher is used here too; the single storage
// definition lives in device_caps.cc (linked alongside this TU).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "vulkan_drm_backend.h"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "backend/drm_kms_vulkan/device_caps.h"
#include "logging.h"

namespace {

const auto& d = vk::detail::defaultDispatchLoaderDynamic;

// Device extensions required for zero-copy dma-buf scanout and explicit
// synchronization. The dependencies of VK_EXT_image_drm_format_modifier
// (bind_memory2, get_memory_requirements2, sampler_ycbcr_conversion,
// get_physical_device_properties2) are all core in Vulkan 1.1, which the
// instance targets, so only the non-core extensions are listed here.
constexpr std::array<const char*, 7> kRequiredDeviceExtensions = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
};

bool HasExt(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

bool LooksLikeSoftware(const char* name) {
  return std::strstr(name, "llvmpipe") != nullptr ||
         std::strstr(name, "lavapipe") != nullptr ||
         std::strstr(name, "SwiftShader") != nullptr;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                   void* /*user_data*/) {
  const char* msg = data && data->pMessage ? data->pMessage : "";
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    spdlog::error("[vulkan] {}", msg);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    spdlog::warn("[vulkan] {}", msg);
  } else {
    spdlog::debug("[vulkan] {}", msg);
  }
  return VK_FALSE;
}

// Matches FlutterVulkanInstanceProcAddressCallback: the instance handle is an
// opaque void* (FlutterVulkanInstanceHandle), cast back to VkInstance here.
void* GetInstanceProcAddressCallback(void* /*user_data*/,
                                     void* instance,
                                     const char* procname) {
  return reinterpret_cast<void*>(
      d.vkGetInstanceProcAddr(static_cast<VkInstance>(instance), procname));
}

}  // namespace

VulkanDrmBackend::VulkanDrmBackend(std::string drm_device,
                                   bool enable_validation,
                                   homescreen::DrmSession* session)
    : drm_device_(std::move(drm_device)),
      enable_validation_(enable_validation),
      session_(session) {}

VulkanDrmBackend::~VulkanDrmBackend() {
  Teardown();
}

std::shared_ptr<VulkanDrmBackend> VulkanDrmBackend::Create(
    const std::string& drm_device,
    bool enable_validation,
    homescreen::DrmSession* session) {
  auto backend = std::shared_ptr<VulkanDrmBackend>(
      new VulkanDrmBackend(drm_device, enable_validation, session));

  std::string refusal;
  if (!backend->BringUp(refusal)) {
    spdlog::critical("[VulkanDrmBackend] init failed; refusing to start: {}",
                     refusal);
    return nullptr;
  }

  // Bring-up succeeded; the device and queues exist and the capabilities are
  // logged. The render and present path is not wired yet, so refuse rather
  // than hand back a backend that cannot present a frame.
  spdlog::critical(
      "[VulkanDrmBackend] device and queues created; the Vulkan render and "
      "present path is not implemented yet. Refusing to start. Use "
      "BUILD_BACKEND_DRM_KMS_EGL for a working DRM/KMS backend.");
  return nullptr;
}

bool VulkanDrmBackend::BringUp(std::string& refusal_reason) {
  try {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();
  } catch (const std::exception& e) {
    refusal_reason = std::string(
                         "Vulkan loader not present (libvulkan could not be "
                         "opened): ") +
                     e.what();
    return false;
  }
  if (d.vkCreateInstance == nullptr) {
    refusal_reason = "Vulkan loader present but vkCreateInstance unresolved";
    return false;
  }

  if (!CreateInstance(refusal_reason)) {
    return false;
  }
  SetupDebugMessenger();
  if (!SelectPhysicalDevice(refusal_reason)) {
    return false;
  }
  if (!CreateLogicalDevice(refusal_reason)) {
    return false;
  }
  PopulateCaps();

  spdlog::info(
      "[VulkanDrmBackend] device='{}' driver='{}' vendor=0x{:04x} "
      "device=0x{:04x} api={}.{}.{}",
      caps_.device_name, caps_.driver_name, caps_.vendor_id, caps_.device_id,
      VK_VERSION_MAJOR(caps_.api_version), VK_VERSION_MINOR(caps_.api_version),
      VK_VERSION_PATCH(caps_.api_version));
  spdlog::info(
      "[VulkanDrmBackend] caps: drm_node={} timeline_sem={} global_priority={} "
      "lazy_transient={} dedicated_transfer={} gfx_queues={} max_image_2d={}",
      caps_.has_physical_device_drm, caps_.has_timeline_semaphore,
      caps_.has_global_priority, caps_.has_lazy_transient,
      caps_.has_dedicated_transfer_queue, caps_.graphics_queue_count,
      caps_.max_image_2d);
  spdlog::info(
      "[VulkanDrmBackend] graphics queue family {} created; scanout node '{}'",
      graphics_queue_family_, drm_device_);
  return true;
}

bool VulkanDrmBackend::CreateInstance(std::string& refusal_reason) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "ivi-homescreen";
  app.apiVersion = VK_API_VERSION_1_1;

  if (enable_validation_) {
    uint32_t layer_count = 0;
    d.vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    if (layer_count > 0) {
      d.vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    }
    constexpr const char* kValidation = "VK_LAYER_KHRONOS_validation";
    for (const auto& l : layers) {
      if (std::strcmp(l.layerName, kValidation) == 0) {
        enabled_instance_layers_.push_back(kValidation);
        enabled_instance_extensions_.push_back(
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        break;
      }
    }
    if (enabled_instance_layers_.empty()) {
      spdlog::warn(
          "[VulkanDrmBackend] validation requested (-d) but "
          "VK_LAYER_KHRONOS_validation is not enumerable; continuing without "
          "validation");
    }
  }

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  info.enabledLayerCount =
      static_cast<uint32_t>(enabled_instance_layers_.size());
  info.ppEnabledLayerNames = enabled_instance_layers_.data();
  info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_instance_extensions_.size());
  info.ppEnabledExtensionNames = enabled_instance_extensions_.data();

  if (d.vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
    refusal_reason = "vkCreateInstance failed";
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
  return true;
}

void VulkanDrmBackend::SetupDebugMessenger() {
  if (enabled_instance_layers_.empty() ||
      d.vkCreateDebugUtilsMessengerEXT == nullptr) {
    return;
  }
  VkDebugUtilsMessengerCreateInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = DebugUtilsCallback;
  d.vkCreateDebugUtilsMessengerEXT(instance_, &info, nullptr,
                                   &debug_messenger_);
}

bool VulkanDrmBackend::SelectPhysicalDevice(std::string& refusal_reason) {
  uint32_t count = 0;
  d.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count > 0) {
    d.vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  }

  unsigned disp_major = 0;
  unsigned disp_minor = 0;
  drm_kms_vulkan::DrmNodeNumber(drm_device_, disp_major, disp_minor);

  uint64_t best_score = 0;
  std::string last_miss;
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props{};
    d.vkGetPhysicalDeviceProperties(pd, &props);

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        LooksLikeSoftware(props.deviceName)) {
      last_miss = std::string(props.deviceName) + " is a CPU/software renderer";
      continue;
    }

    uint32_t ext_count = 0;
    d.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    if (ext_count > 0) {
      d.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count,
                                             exts.data());
    }
    bool has_all = true;
    for (const char* req : kRequiredDeviceExtensions) {
      if (!HasExt(exts, req)) {
        has_all = false;
        last_miss = std::string(props.deviceName) + " missing " + req;
        break;
      }
    }
    if (!has_all) {
      continue;
    }

    uint32_t qf_count = 0;
    d.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    if (qf_count > 0) {
      d.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
    }
    uint32_t gfx_family = UINT32_MAX;
    for (uint32_t i = 0; i < qfs.size(); ++i) {
      if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        gfx_family = i;
        break;
      }
    }
    if (gfx_family == UINT32_MAX) {
      last_miss = std::string(props.deviceName) + " has no graphics queue";
      continue;
    }

    // A DRM-node match against the scanout device dominates (render on the GPU
    // that drives the display); then prefer discrete GPUs; then a larger max
    // image dimension.
    uint64_t score = 1;
    if (HasExt(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) &&
        (disp_major != 0 || disp_minor != 0)) {
      VkPhysicalDeviceDrmPropertiesEXT drm{};
      drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
      VkPhysicalDeviceProperties2 p2{};
      p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p2.pNext = &drm;
      d.vkGetPhysicalDeviceProperties2(pd, &p2);
      const bool primary_match =
          drm.hasPrimary &&
          static_cast<unsigned>(drm.primaryMajor) == disp_major &&
          static_cast<unsigned>(drm.primaryMinor) == disp_minor;
      const bool render_match =
          drm.hasRender &&
          static_cast<unsigned>(drm.renderMajor) == disp_major &&
          static_cast<unsigned>(drm.renderMinor) == disp_minor;
      if (primary_match || render_match) {
        score += 1ULL << 40;
      }
    }
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score += 1ULL << 30;
    }
    score += props.limits.maxImageDimension2D;

    if (score > best_score) {
      best_score = score;
      physical_device_ = pd;
      graphics_queue_family_ = gfx_family;
      enabled_device_extensions_.assign(kRequiredDeviceExtensions.begin(),
                                        kRequiredDeviceExtensions.end());
    }
  }

  if (physical_device_ == VK_NULL_HANDLE) {
    refusal_reason =
        "no Vulkan physical device supports zero-copy dma-buf scanout" +
        (last_miss.empty() ? std::string()
                           : std::string(" (") + last_miss + ")");
    return false;
  }
  return true;
}

bool VulkanDrmBackend::CreateLogicalDevice(std::string& refusal_reason) {
  // Query which optional sync features the selected device offers so the
  // feature chain only enables what is present.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_supported{};
  timeline_supported.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  VkPhysicalDeviceSynchronization2Features sync2_supported{};
  sync2_supported.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2_supported.pNext = &timeline_supported;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &sync2_supported;
  d.vkGetPhysicalDeviceFeatures2(physical_device_, &features2);

  if (sync2_supported.synchronization2 != VK_TRUE) {
    refusal_reason = "selected device does not support synchronization2";
    return false;
  }

  VkPhysicalDeviceSynchronization2Features sync2_enable{};
  sync2_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2_enable.synchronization2 = VK_TRUE;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_enable{};
  timeline_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  timeline_enable.timelineSemaphore = VK_TRUE;
  if (timeline_supported.timelineSemaphore == VK_TRUE) {
    sync2_enable.pNext = &timeline_enable;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = graphics_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures device_features{};
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.pNext = &sync2_enable;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();
  device_info.pEnabledFeatures = &device_features;

  if (d.vkCreateDevice(physical_device_, &device_info, nullptr, &device_) !=
      VK_SUCCESS) {
    refusal_reason = "vkCreateDevice failed";
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));
  d.vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
  return true;
}

void VulkanDrmBackend::PopulateCaps() {
  VkPhysicalDeviceProperties props{};
  d.vkGetPhysicalDeviceProperties(physical_device_, &props);
  caps_.device_name = props.deviceName;
  caps_.vendor_id = props.vendorID;
  caps_.device_id = props.deviceID;
  caps_.api_version = props.apiVersion;
  caps_.max_image_2d = props.limits.maxImageDimension2D;

  uint32_t ext_count = 0;
  d.vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count,
                                         nullptr);
  std::vector<VkExtensionProperties> exts(ext_count);
  if (ext_count > 0) {
    d.vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                           &ext_count, exts.data());
  }
  caps_.has_physical_device_drm =
      HasExt(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
  caps_.has_global_priority = HasExt(exts, "VK_EXT_global_priority") ||
                              HasExt(exts, "VK_KHR_global_priority");

  // Query the actual timelineSemaphore feature bit rather than inferring it
  // from the API version or extension list — Mesa Turnip, for one, exposes it
  // on a device whose advertised properties would not imply it.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
  timeline.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  VkPhysicalDeviceFeatures2 timeline_features2{};
  timeline_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  timeline_features2.pNext = &timeline;
  d.vkGetPhysicalDeviceFeatures2(physical_device_, &timeline_features2);
  caps_.has_timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;

  VkPhysicalDeviceDriverProperties driver{};
  driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  p2.pNext = &driver;
  d.vkGetPhysicalDeviceProperties2(physical_device_, &p2);
  caps_.driver_name = driver.driverName;

  VkPhysicalDeviceMemoryProperties mem{};
  d.vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if (mem.memoryTypes[i].propertyFlags &
        VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
      caps_.has_lazy_transient = true;
      break;
    }
  }

  uint32_t qf_count = 0;
  d.vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count,
                                             nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  if (qf_count > 0) {
    d.vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count,
                                               qfs.data());
  }
  if (graphics_queue_family_ < qfs.size()) {
    caps_.graphics_queue_count = qfs[graphics_queue_family_].queueCount;
  }
  for (const auto& qf : qfs) {
    const bool transfer = (qf.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
    const bool graphics = (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    const bool compute = (qf.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    if (transfer && !graphics && !compute) {
      caps_.has_dedicated_transfer_queue = true;
      break;
    }
  }

  caps_.zero_copy_supported = true;
}

void VulkanDrmBackend::Teardown() {
  if (device_ != VK_NULL_HANDLE) {
    d.vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (debug_messenger_ != VK_NULL_HANDLE &&
      d.vkDestroyDebugUtilsMessengerEXT != nullptr) {
    d.vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    debug_messenger_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    d.vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

// ── Backend interface
// ───────────────────────────────────────────────────────── Present/scanout is
// not wired yet; never reached while Create() refuses, but required for the
// vtable and the FlutterView call sites.

void VulkanDrmBackend::Resize(size_t /*index*/,
                              Engine* /*flutter_engine*/,
                              int32_t /*width*/,
                              int32_t /*height*/) {}

void VulkanDrmBackend::CreateSurface(size_t /*index*/,
                                     struct wl_surface* /*surface*/,
                                     int32_t /*width*/,
                                     int32_t /*height*/) {}

bool VulkanDrmBackend::TextureMakeCurrent() {
  return false;
}

bool VulkanDrmBackend::TextureClearCurrent() {
  return false;
}

FlutterRendererConfig VulkanDrmBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kVulkan;
  config.vulkan.struct_size = sizeof(FlutterVulkanRendererConfig);
  config.vulkan.version = VK_MAKE_VERSION(1, 1, 0);
  config.vulkan.instance = instance_;
  config.vulkan.physical_device = physical_device_;
  config.vulkan.device = device_;
  config.vulkan.queue_family_index = graphics_queue_family_;
  config.vulkan.queue = graphics_queue_;
  config.vulkan.enabled_instance_extension_count =
      enabled_instance_extensions_.size();
  config.vulkan.enabled_instance_extensions =
      enabled_instance_extensions_.data();
  config.vulkan.enabled_device_extension_count =
      enabled_device_extensions_.size();
  config.vulkan.enabled_device_extensions = enabled_device_extensions_.data();
  config.vulkan.get_instance_proc_address_callback =
      GetInstanceProcAddressCallback;
  return config;
}

FlutterCompositor VulkanDrmBackend::GetCompositorConfig() {
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  return compositor;
}
