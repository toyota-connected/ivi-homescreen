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

// Use vulkan.hpp's dynamic dispatch loader, matching the Wayland-Vulkan
// backend. The loader dlopens libvulkan at runtime, so a missing loader (the
// misconfigured-Yocto / pre-driver-install image the gate guards against)
// surfaces as a catchable failure here rather than an unresolved-symbol link
// error. This TU owns the single dynamic-dispatcher storage definition for the
// drm_kms_vulkan backend (Wayland-Vulkan's wayland_vulkan.cc owns the other;
// the two backends are mutually exclusive, so exactly one is ever linked).
//
// System headers are included before vulkan.hpp so the stat()/sysmacros use in
// DrmNodeNumber resolves cleanly.
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "device_caps.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace drm_kms_vulkan {

namespace {

const auto& d = vk::detail::defaultDispatchLoaderDynamic;

bool HasExt(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

bool NameLooksLikeSoftware(const char* name) {
  // llvmpipe reports deviceType == CPU (already rejected), but lavapipe builds
  // and some odd configs slip through; a name match is a cheap backstop.
  return std::strstr(name, "llvmpipe") != nullptr ||
         std::strstr(name, "lavapipe") != nullptr ||
         std::strstr(name, "SwiftShader") != nullptr;
}

}  // namespace

DeviceCaps ProbeDeviceCaps(const std::string& display_device,
                           std::string& refusal_reason) {
  DeviceCaps caps{};

  // Bootstrap the loader. The no-arg init() resolves vkGetInstanceProcAddr +
  // the global-level entry points by dlopening libvulkan.
  try {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();
  } catch (const std::exception& e) {
    refusal_reason = std::string(
                         "Vulkan loader not present (libvulkan could not be "
                         "opened): ") +
                     e.what();
    return caps;
  }
  if (d.vkCreateInstance == nullptr) {
    refusal_reason = "Vulkan loader present but vkCreateInstance unresolved";
    return caps;
  }

  // Minimal instance. Vulkan 1.1 gives core get_physical_device_properties2 and
  // queue_family_foreign without instance extensions.
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "ivi-homescreen drm_kms_vulkan probe";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  if (d.vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    refusal_reason = "vkCreateInstance failed";
    return caps;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance));

  // Walk physical devices; the first non-CPU, non-software device that exposes
  // the full dma-buf-import extension set wins the gate.
  uint32_t count = 0;
  d.vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count > 0) {
    d.vkEnumeratePhysicalDevices(instance, &count, devices.data());
  }

  static constexpr const char* kRequired[] = {
      "VK_EXT_image_drm_format_modifier",
      "VK_EXT_external_memory_dma_buf",
      "VK_KHR_external_memory_fd",
      "VK_EXT_queue_family_foreign",
  };

  bool found = false;
  std::string last_miss;
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props{};
    d.vkGetPhysicalDeviceProperties(pd, &props);

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        NameLooksLikeSoftware(props.deviceName)) {
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

    bool ok = true;
    for (const char* req : kRequired) {
      if (!HasExt(exts, req)) {
        ok = false;
        last_miss =
            std::string(props.deviceName) + " missing " + std::string(req);
        break;
      }
    }
    if (!ok) {
      continue;
    }

    // Gate passed for this device — record caps (display openability checked
    // below decides zero_copy_supported).
    caps.device_name = props.deviceName;
    caps.vendor_id = props.vendorID;
    caps.device_id = props.deviceID;
    caps.api_version = props.apiVersion;
    caps.max_image_2d = props.limits.maxImageDimension2D;
    caps.has_physical_device_drm = HasExt(exts, "VK_EXT_physical_device_drm");
    caps.has_global_priority = HasExt(exts, "VK_EXT_global_priority") ||
                               HasExt(exts, "VK_KHR_global_priority");

    // Query the actual timelineSemaphore feature bit rather than inferring it
    // from the API version or extension list — Mesa Turnip, for one, exposes
    // it on a device whose advertised properties would not imply it.
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &timeline;
    d.vkGetPhysicalDeviceFeatures2(pd, &features2);
    caps.has_timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;

    VkPhysicalDeviceMemoryProperties mem{};
    d.vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
      if (mem.memoryTypes[i].propertyFlags &
          VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
        caps.has_lazy_transient = true;
        break;
      }
    }

    uint32_t qf_count = 0;
    d.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    if (qf_count > 0) {
      d.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
    }
    for (const auto& qf : qfs) {
      if (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        caps.graphics_queue_count = qf.queueCount;
        break;
      }
    }
    for (const auto& qf : qfs) {
      const bool transfer = (qf.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
      const bool graphics = (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
      const bool compute = (qf.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
      if (transfer && !graphics && !compute) {
        caps.has_dedicated_transfer_queue = true;
        break;
      }
    }

    found = true;
    break;
  }

  d.vkDestroyInstance(instance, nullptr);

  if (!found) {
    refusal_reason =
        "no Vulkan physical device exposes the zero-copy dma-buf import "
        "extension set" +
        (last_miss.empty() ? std::string()
                           : std::string(" (") + last_miss + ")");
    return caps;
  }

  // The display node must be openable — a render-only GPU with no display DRM
  // node cannot scan out. open()+close() is the cheapest probe.
  const int fd = ::open(display_device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    refusal_reason = "DRM display node '" + display_device +
                     "' is not openable: " + std::strerror(errno);
    return caps;
  }
  ::close(fd);

  caps.zero_copy_supported = true;
  return caps;
}

bool DrmNodeNumber(const std::string& path,
                   unsigned& major_out,
                   unsigned& minor_out) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  // vulkan.hpp #undefs the major()/minor() macros (they clash with version
  // field names), so call the underlying glibc functions directly.
  major_out = gnu_dev_major(st.st_rdev);
  minor_out = gnu_dev_minor(st.st_rdev);
  return true;
}

}  // namespace drm_kms_vulkan
