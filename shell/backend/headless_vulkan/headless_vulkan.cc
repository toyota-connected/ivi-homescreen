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

// Use vulkan.hpp's dynamic dispatch loader, matching the other Vulkan backends.
// The loader dlopens libvulkan at runtime, so no Vulkan link library is needed
// — only the vendored headers at build time.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "backend/headless_vulkan/headless_vulkan.h"

// The vulkan.hpp dynamic dispatcher needs its storage defined in exactly ONE TU
// per binary. When the drm-kms-vulkan backend is compiled in, its
// device_caps.cc owns the single definition; when the wayland-vulkan backend is
// compiled in (without drm-kms-vulkan), wayland_vulkan.cc owns it. Only when
// neither is present does this TU own it. vulkan.hpp still declares
// vk::detail::defaultDispatchLoaderDynamic extern, so d() below resolves
// against whichever TU defined the storage.
#if !BUILD_BACKEND_DRM_KMS_VULKAN && !BUILD_BACKEND_WAYLAND_VULKAN
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "logging/logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "task_runner.h"

// DRM fourcc / modifier constants for the dmabuf export path. Defined locally
// (mirroring how drm_kms_vulkan sources these from <drm_fourcc.h>) so the
// headless_vulkan CMake block keeps its single vulkan_headers dependency and
// never pulls in libdrm — the only two values this backend needs.
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

namespace {

// Accessor for the dynamic dispatcher (storage owned by whichever Vulkan TU is
// compiled in — see above). A function rather than a namespace-scope reference
// so there is no static-init-order dependency across TUs.
const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

HeadlessVulkanBackend* BackendOf(void* user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  return reinterpret_cast<HeadlessVulkanBackend*>(
      state->view_controller->engine->GetBackend());
}

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

// Parse a hex UUID string (e.g. from IVI_VK_DEVICE_UUID) into up to
// VK_UUID_SIZE bytes, ignoring any non-hex separators (dashes, spaces). Returns
// the number of bytes filled.
size_t ParseHexUuid(const char* s, std::array<uint8_t, VK_UUID_SIZE>& out) {
  size_t bytes = 0;
  int hi = -1;
  for (const char* p = s; *p != '\0' && bytes < out.size(); ++p) {
    int nibble;
    if (*p >= '0' && *p <= '9') {
      nibble = *p - '0';
    } else if (*p >= 'a' && *p <= 'f') {
      nibble = *p - 'a' + 10;
    } else if (*p >= 'A' && *p <= 'F') {
      nibble = *p - 'A' + 10;
    } else {
      continue;  // skip separators
    }
    if (hi < 0) {
      hi = nibble;
    } else {
      out[bytes++] = static_cast<uint8_t>((hi << 4) | nibble);
      hi = -1;
    }
  }
  return bytes;
}

// Usage the engine composites the render targets with — kept identical between
// the plain and exportable pools so a consumer recreates a matching image.
constexpr VkImageUsageFlags kRenderTargetUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

// DRM fourcc for VK_FORMAT_R8G8B8A8_UNORM. DRM fourccs name the byte order in
// memory (R,G,B,A ascending) which reversed is ABGR — DRM_FORMAT_ABGR8888,
// fourcc_code('A','B','G','R'). Built locally to avoid a libdrm dependency.
constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}
constexpr uint32_t kDrmFormatAbgr8888 = FourCC('A', 'B', 'G', 'R');

// True if the device can EXPORT @p handle for an image of this
// format/usage/tiling (and, for the DRM-modifier path, this modifier). Uses the
// vkGetPhysicalDeviceImageFormatProperties2 external-image query and checks
// VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT — the robustness gate before any
// exportable VkImage is created.
bool ExternalHandleExportable(VkPhysicalDevice phys,
                              VkFormat fmt,
                              VkImageUsageFlags usage,
                              VkImageTiling tiling,
                              VkExternalMemoryHandleTypeFlagBits handle,
                              uint64_t modifier,
                              bool has_modifier) {
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT dm{};
  dm.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
  dm.drmFormatModifier = modifier;
  dm.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkPhysicalDeviceExternalImageFormatInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  ext.handleType = handle;
  if (has_modifier) {
    ext.pNext = &dm;
  }

  VkPhysicalDeviceImageFormatInfo2 ifi{};
  ifi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  ifi.pNext = &ext;
  ifi.format = fmt;
  ifi.type = VK_IMAGE_TYPE_2D;
  ifi.tiling = tiling;
  ifi.usage = usage;
  ifi.flags = 0;

  VkExternalImageFormatProperties efp{};
  efp.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  p2.pNext = &efp;

  if (d().vkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &p2) !=
      VK_SUCCESS) {
    return false;
  }
  return (efp.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}

// Plane count the driver uses for @p modifier with @p fmt (adapted from
// drm_kms_vulkan's PlaneCountForModifier — the 2-call format-properties2
// pattern).
uint32_t PlaneCountForModifier(VkPhysicalDevice phys,
                               VkFormat fmt,
                               uint64_t modifier) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  d().vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  d().vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
  for (const auto& m : mods) {
    if (m.drmFormatModifier == modifier) {
      return m.drmFormatModifierPlaneCount;
    }
  }
  return 0;
}

// The DRM modifiers the device can EXPORT as a dma-buf for @p fmt with this
// usage. Enumerates every modifier the format advertises, then keeps only those
// that pass the exportable query above (which also validates the usage for the
// DRM-modifier tiling). The image is created constrained to this list and the
// driver picks the concrete modifier, read back after bind.
std::vector<uint64_t> ExportableModifiers(VkPhysicalDevice phys,
                                          VkFormat fmt,
                                          VkImageUsageFlags usage) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  d().vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  d().vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);

  std::vector<uint64_t> out;
  for (const auto& m : mods) {
    if (ExternalHandleExportable(phys, fmt, usage,
                                 VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                 m.drmFormatModifier, /*has_modifier=*/true)) {
      out.push_back(m.drmFormatModifier);
    }
  }
  return out;
}

}  // namespace

HeadlessVulkanBackend::HeadlessVulkanBackend(const uint32_t width,
                                             const uint32_t height)
    : width_(width), height_(height) {
  // Pace the engine to IVI_HEADLESS_FPS (default 30) via a synthetic vsync, so
  // it does not free-run wall-clock without a display. fps <= 0 disables it and
  // leaves Flutter's wall-clock scheduler.
  const char* fps_env = std::getenv("IVI_HEADLESS_FPS");
  const int fps = fps_env != nullptr ? std::atoi(fps_env) : 30;
  if (fps > 0) {
    vsync_period_ns_ = static_cast<uint32_t>(1'000'000'000LL / fps);
  }

  // IVI_VK_DEVICE_UUID (hex, optional): pin the physical device by its
  // deviceUUID rather than taking the first non-CPU one.
  if (const char* uuid = std::getenv("IVI_VK_DEVICE_UUID");
      uuid != nullptr && uuid[0] != '\0') {
    if (ParseHexUuid(uuid, wanted_uuid_) == VK_UUID_SIZE) {
      have_wanted_uuid_ = true;
    } else {
      ihs::log::warn(
          "[HeadlessVulkan] IVI_VK_DEVICE_UUID '{}' is not 16 hex bytes; "
          "ignoring",
          uuid);
    }
  }

  // IVI_VK_EXPORT_MODE (opt-in): make the render-target pool exportable as
  // external-memory dma-bufs ("dmabuf") or opaque fds ("opaque"). Unset leaves
  // the current non-exportable pool, so the default boot needs no
  // export-capable device.
  if (const char* mode = std::getenv("IVI_VK_EXPORT_MODE");
      mode != nullptr && mode[0] != '\0') {
    if (std::strcmp(mode, "opaque") == 0) {
      export_mode_ = ExportMode::kOpaqueFd;
    } else if (std::strcmp(mode, "dmabuf") == 0) {
      export_mode_ = ExportMode::kDmaBuf;
    } else {
      ihs::log::warn(
          "[HeadlessVulkan] IVI_VK_EXPORT_MODE '{}' unknown (want "
          "opaque|dmabuf); export disabled",
          mode);
    }
  }

  if (!InitVulkan()) {
    ihs::log::error("[HeadlessVulkan] Vulkan init failed; backend inert");
    Teardown();
  }
}

HeadlessVulkanBackend::~HeadlessVulkanBackend() {
  Teardown();
}

bool HeadlessVulkanBackend::InitVulkan() {
  // Bootstrap the loader. The no-arg init() resolves vkGetInstanceProcAddr +
  // the global-level entry points by dlopening libvulkan.
  try {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();
  } catch (const std::exception& e) {
    ihs::log::error("[HeadlessVulkan] Vulkan loader not present: {}", e.what());
    return false;
  }
  if (d().vkCreateInstance == nullptr) {
    ihs::log::error("[HeadlessVulkan] vkCreateInstance unresolved");
    return false;
  }
  return CreateInstance() && SelectPhysicalDevice() && CreateLogicalDevice() &&
         CreateRenderTargets();
}

bool HeadlessVulkanBackend::CreateInstance() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "ivi-homescreen headless_vulkan";
  app.apiVersion = VK_API_VERSION_1_1;

  // Optional instance extensions for a later export path: enable them only when
  // the loader advertises them so this stays minimal and cross-driver safe.
  uint32_t ext_count = 0;
  d().vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail(ext_count);
  if (ext_count > 0) {
    d().vkEnumerateInstanceExtensionProperties(nullptr, &ext_count,
                                               avail.data());
  }
  for (const char* opt :
       {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME}) {
    if (HasExt(avail, opt)) {
      enabled_instance_extensions_.push_back(opt);
    }
  }

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_instance_extensions_.size());
  info.ppEnabledExtensionNames = enabled_instance_extensions_.data();

  if (d().vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] vkCreateInstance failed");
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
  return true;
}

bool HeadlessVulkanBackend::SelectPhysicalDevice() {
  uint32_t count = 0;
  d().vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count > 0) {
    d().vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  }

  std::string last_miss;
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props{};
    d().vkGetPhysicalDeviceProperties(pd, &props);

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        LooksLikeSoftware(props.deviceName)) {
      last_miss = std::string(props.deviceName) + " is a CPU/software renderer";
      continue;
    }
    // The Flutter Vulkan renderer requires a 1.1 device.
    if (props.apiVersion < VK_API_VERSION_1_1) {
      last_miss = std::string(props.deviceName) + " exposes Vulkan < 1.1";
      continue;
    }

    // The device's deviceUUID: needed both to honor IVI_VK_DEVICE_UUID and to
    // retain it for a future export handshake.
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &id;
    d().vkGetPhysicalDeviceProperties2(pd, &p2);

    if (have_wanted_uuid_ &&
        std::memcmp(id.deviceUUID, wanted_uuid_.data(), VK_UUID_SIZE) != 0) {
      last_miss = std::string(props.deviceName) + " deviceUUID mismatch";
      continue;
    }

    uint32_t qf_count = 0;
    d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    if (qf_count > 0) {
      d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
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

    physical_device_ = pd;
    graphics_queue_family_ = gfx_family;
    std::memcpy(device_uuid_.data(), id.deviceUUID, VK_UUID_SIZE);
    ihs::log::info("[HeadlessVulkan] selected device '{}'", props.deviceName);
    break;
  }

  if (physical_device_ == VK_NULL_HANDLE) {
    ihs::log::error("[HeadlessVulkan] no usable Vulkan device{}",
                    last_miss.empty() ? std::string()
                                      : std::string(" (") + last_miss + ")");
    return false;
  }
  return true;
}

bool HeadlessVulkanBackend::CreateLogicalDevice() {
  // Enable the external-memory / external-semaphore device extensions when the
  // device advertises them. Harmless in WS-1; a later export path needs them.
  uint32_t ext_count = 0;
  d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                           &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail(ext_count);
  if (ext_count > 0) {
    d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                             &ext_count, avail.data());
  }
  for (const char* opt :
       {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        // dma-buf export path (opt-in via IVI_VK_EXPORT_MODE=dmabuf): the
        // DRM-format-modifier tiling + dma-buf handle type. Enabled when
        // present so the export preflight can succeed; harmless otherwise.
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}) {
    if (HasExt(avail, opt)) {
      enabled_device_extensions_.push_back(opt);
    }
  }

  constexpr float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = graphics_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();

  if (d().vkCreateDevice(physical_device_, &device_info, nullptr, &device_) !=
      VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] vkCreateDevice failed");
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));
  d().vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
  return true;
}

uint32_t HeadlessVulkanBackend::FindDeviceLocalMemoryType(
    const uint32_t type_bits) const {
  VkPhysicalDeviceMemoryProperties mem_props{};
  d().vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) != 0 &&
        (mem_props.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool HeadlessVulkanBackend::CreateRenderTargets() {
  // Decide the effective export mode. When export is requested, preflight the
  // device: the needed extensions must be enabled and the device must advertise
  // EXPORTABLE for the requested handle type / tiling. If anything is missing,
  // warn and fall through to the plain (non-exportable) pool so the backend
  // still boots and paces — export is never allowed to hard-fail bring-up.
  active_export_mode_ = ExportMode::kNone;
  std::vector<uint64_t> allowed_modifiers;

  if (export_mode_ != ExportMode::kNone) {
    auto ext_enabled = [&](const char* name) {
      for (const char* e : enabled_device_extensions_) {
        if (std::strcmp(e, name) == 0) {
          return true;
        }
      }
      return false;
    };

    if (export_mode_ == ExportMode::kOpaqueFd) {
      if (!ext_enabled(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ||
          !ext_enabled(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        ihs::log::warn(
            "[HeadlessVulkan] opaque-fd export requested but "
            "VK_KHR_external_memory[_fd] absent; using non-exportable pool");
      } else if (!ExternalHandleExportable(
                     physical_device_, image_format_, kRenderTargetUsage,
                     VK_IMAGE_TILING_OPTIMAL,
                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                     /*modifier=*/0, /*has_modifier=*/false)) {
        ihs::log::warn(
            "[HeadlessVulkan] device does not advertise OPAQUE_FD export for "
            "this format/usage; using non-exportable pool");
      } else {
        active_export_mode_ = ExportMode::kOpaqueFd;
      }
    } else {  // kDmaBuf
      if (!ext_enabled(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ||
          !ext_enabled(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) ||
          !ext_enabled(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        ihs::log::warn(
            "[HeadlessVulkan] dmabuf export requested but "
            "VK_EXT_external_memory_dma_buf / "
            "VK_EXT_image_drm_format_modifier / VK_KHR_external_memory_fd "
            "absent; using non-exportable pool");
      } else {
        allowed_modifiers = ExportableModifiers(physical_device_, image_format_,
                                                kRenderTargetUsage);
        if (allowed_modifiers.empty()) {
          ihs::log::warn(
              "[HeadlessVulkan] no DRM modifier for this format/usage is "
              "dma-buf exportable; using non-exportable pool");
        } else {
          active_export_mode_ = ExportMode::kDmaBuf;
        }
      }
    }
  }

  if (active_export_mode_ != ExportMode::kNone) {
    if (CreateExportablePool(allowed_modifiers)) {
      ihs::log::info(
          "[HeadlessVulkan] export pool active: mode={} images={} {}x{}",
          active_export_mode_ == ExportMode::kDmaBuf ? "dmabuf" : "opaque",
          kImageCount, width_, height_);
      return true;
    }
    // Exportable creation failed after a passing preflight (driver-specific);
    // tear down any partial pool and fall back to the plain path.
    ihs::log::warn(
        "[HeadlessVulkan] exportable pool creation failed; falling back to "
        "non-exportable pool");
    DestroyRenderTargets();
    active_export_mode_ = ExportMode::kNone;
  }

  if (!CreatePlainPool()) {
    return false;
  }
  ihs::log::info("[HeadlessVulkan] {} render targets ready {}x{}", kImageCount,
                 width_, height_);
  return true;
}

bool HeadlessVulkanBackend::CreatePlainPool() {
  for (uint32_t i = 0; i < kImageCount; ++i) {
    if (!CreatePlainImage(i)) {
      return false;
    }
  }
  return true;
}

bool HeadlessVulkanBackend::CreatePlainImage(const uint32_t i) {
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = image_format_;
  ici.extent = {width_, height_, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = kRenderTargetUsage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (d().vkCreateImage(device_, &ici, nullptr, &images_[i]) != VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] vkCreateImage {} failed", i);
    return false;
  }

  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device_, images_[i], &req);
  const uint32_t type_index = FindDeviceLocalMemoryType(req.memoryTypeBits);
  if (type_index == UINT32_MAX) {
    ihs::log::error("[HeadlessVulkan] no DEVICE_LOCAL memory type for image");
    return false;
  }

  // Dedicated allocation per image — one image per allocation is fine for the
  // small ring and keeps the export path (one dma-buf per image) simple.
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = images_[i];
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &dedicated;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;

  if (d().vkAllocateMemory(device_, &alloc, nullptr, &image_memory_[i]) !=
      VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] vkAllocateMemory {} failed", i);
    return false;
  }
  if (d().vkBindImageMemory(device_, images_[i], image_memory_[i], 0) !=
      VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] vkBindImageMemory {} failed", i);
    return false;
  }
  return true;
}

bool HeadlessVulkanBackend::CreateExportablePool(
    const std::vector<uint64_t>& allowed_modifiers) {
  for (uint32_t i = 0; i < kImageCount; ++i) {
    if (!CreateExportableImage(i, allowed_modifiers)) {
      return false;
    }
  }
  return true;
}

bool HeadlessVulkanBackend::CreateExportableImage(
    const uint32_t i,
    const std::vector<uint64_t>& allowed_modifiers) {
  const bool dmabuf = active_export_mode_ == ExportMode::kDmaBuf;
  const VkExternalMemoryHandleTypeFlagBits handle_type =
      dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
             : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  // Constrain the dma-buf image to the negotiated modifier set (driver picks
  // one, read back after bind). Unused for opaque-fd, which keeps OPTIMAL.
  VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
  mod_list.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  mod_list.drmFormatModifierCount =
      static_cast<uint32_t>(allowed_modifiers.size());
  mod_list.pDrmFormatModifiers = allowed_modifiers.data();

  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = handle_type;
  if (dmabuf) {
    ext.pNext = &mod_list;
  }

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.pNext = &ext;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = image_format_;
  ici.extent = {width_, height_, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = dmabuf ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                      : VK_IMAGE_TILING_OPTIMAL;
  ici.usage = kRenderTargetUsage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (d().vkCreateImage(device_, &ici, nullptr, &images_[i]) != VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] exportable vkCreateImage {} failed", i);
    return false;
  }

  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device_, images_[i], &req);
  const uint32_t type_index = FindDeviceLocalMemoryType(req.memoryTypeBits);
  if (type_index == UINT32_MAX) {
    ihs::log::error("[HeadlessVulkan] no DEVICE_LOCAL memory type for image");
    return false;
  }

  // Dedicated + exportable allocation: chain the export handle-type info
  // alongside the dedicated-alloc info the plain pool already uses.
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = images_[i];
  VkExportMemoryAllocateInfo emai{};
  emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  emai.handleTypes = handle_type;
  emai.pNext = &dedicated;
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &emai;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;

  if (d().vkAllocateMemory(device_, &alloc, nullptr, &image_memory_[i]) !=
      VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] exportable vkAllocateMemory {} failed",
                    i);
    return false;
  }
  if (d().vkBindImageMemory(device_, images_[i], image_memory_[i], 0) !=
      VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] exportable vkBindImageMemory {} failed",
                    i);
    return false;
  }

  ExportedImage& e = exported_images_[i];
  e.alloc_size = req.size;
  e.memory_type_index = type_index;
  e.width = width_;
  e.height = height_;
  e.vk_format = static_cast<uint32_t>(image_format_);
  e.handle_type = static_cast<uint32_t>(handle_type);
  e.plane_count = 1;

  if (dmabuf) {
    // The concrete modifier the driver chose, plus per-plane offset/pitch a
    // consumer needs to import (aspect VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT).
    VkImageDrmFormatModifierPropertiesEXT mp{};
    mp.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (d().vkGetImageDrmFormatModifierPropertiesEXT(device_, images_[i],
                                                     &mp) != VK_SUCCESS) {
      ihs::log::error(
          "[HeadlessVulkan] vkGetImageDrmFormatModifierPropertiesEXT {} failed",
          i);
      return false;
    }
    e.drm_modifier = mp.drmFormatModifier;
    e.drm_fourcc = kDrmFormatAbgr8888;

    uint32_t plane_count =
        PlaneCountForModifier(physical_device_, image_format_, e.drm_modifier);
    if (plane_count < 1 || plane_count > 4) {
      ihs::log::error(
          "[HeadlessVulkan] unexpected plane count {} for chosen modifier",
          plane_count);
      return false;
    }
    e.plane_count = plane_count;

    static constexpr std::array<VkImageAspectFlagBits, 4> kMemPlane = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
    };
    for (uint32_t p = 0; p < plane_count; ++p) {
      VkImageSubresource sub{};
      sub.aspectMask = kMemPlane[p];
      VkSubresourceLayout sl{};
      d().vkGetImageSubresourceLayout(device_, images_[i], &sub, &sl);
      e.plane_offset[p] = sl.offset;
      e.plane_pitch[p] = sl.rowPitch;
    }
  }

  // Export the fd last, once the image + memory are fully described.
  VkMemoryGetFdInfoKHR gfi{};
  gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  gfi.memory = image_memory_[i];
  gfi.handleType = handle_type;
  if (d().vkGetMemoryFdKHR(device_, &gfi, &e.fd) != VK_SUCCESS) {
    ihs::log::error("[HeadlessVulkan] vkGetMemoryFdKHR {} failed", i);
    return false;
  }

  if (dmabuf) {
    ihs::log::info(
        "[HeadlessVulkan] exported image {}: fd={} size={} fourcc=0x{:08x} "
        "modifier=0x{:016x} planes={}",
        i, e.fd, e.alloc_size, e.drm_fourcc, e.drm_modifier, e.plane_count);
  } else {
    ihs::log::info(
        "[HeadlessVulkan] exported image {}: fd={} size={} memtype={} "
        "(opaque-fd)",
        i, e.fd, e.alloc_size, e.memory_type_index);
  }
  return true;
}

void HeadlessVulkanBackend::DestroyRenderTargets() {
  for (uint32_t i = 0; i < kImageCount; ++i) {
    if (images_[i] != VK_NULL_HANDLE) {
      d().vkDestroyImage(device_, images_[i], nullptr);
      images_[i] = VK_NULL_HANDLE;
    }
    if (image_memory_[i] != VK_NULL_HANDLE) {
      d().vkFreeMemory(device_, image_memory_[i], nullptr);
      image_memory_[i] = VK_NULL_HANDLE;
    }
    if (exported_images_[i].fd >= 0) {
      ::close(exported_images_[i].fd);
    }
    exported_images_[i] = ExportedImage{};
  }
}

void HeadlessVulkanBackend::Teardown() {
  if (device_ != VK_NULL_HANDLE) {
    d().vkDeviceWaitIdle(device_);
    DestroyRenderTargets();
    d().vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  } else {
    // Device already gone (or never created): still close any exported fds.
    for (uint32_t i = 0; i < kImageCount; ++i) {
      if (exported_images_[i].fd >= 0) {
        ::close(exported_images_[i].fd);
        exported_images_[i].fd = -1;
      }
    }
  }
  if (instance_ != VK_NULL_HANDLE) {
    d().vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

// ── Renderer config ──────────────────────────────────────────────────────────

FlutterRendererConfig HeadlessVulkanBackend::GetRenderConfig() {
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
  config.vulkan.get_next_image_callback = GetNextImageCallback;
  config.vulkan.present_image_callback = PresentCallback;
  return config;
}

FlutterCompositor HeadlessVulkanBackend::GetCompositorConfig() {
  // Single-surface rendering: no platform-view layer compositing in WS-1.
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  return compositor;
}

bool HeadlessVulkanBackend::GetVulkanContext(BackendVulkanContext* out) const {
  if (out == nullptr || device_ == VK_NULL_HANDLE) {
    return false;
  }
  out->instance = instance_;
  out->physical_device = physical_device_;
  out->device = device_;
  out->queue = graphics_queue_;
  out->queue_family_index = graphics_queue_family_;
  out->get_instance_proc_addr =
      reinterpret_cast<void*>(d().vkGetInstanceProcAddr);
  out->device_extensions = enabled_device_extensions_.data();
  out->device_extension_count = enabled_device_extensions_.size();
  return true;
}

void* HeadlessVulkanBackend::GetInstanceProcAddressCallback(
    void* /* user_data */,
    FlutterVulkanInstanceHandle instance,
    const char* procname) {
  auto* vk_instance = static_cast<VkInstance>(instance);
  return reinterpret_cast<void*>(
      d().vkGetInstanceProcAddr(vk_instance, procname));
}

FlutterVulkanImage HeadlessVulkanBackend::GetNextImageCallback(
    void* user_data,
    const FlutterFrameInfo* /* frame_info */) {
  auto* backend = BackendOf(user_data);
  return {
      .struct_size = sizeof(FlutterVulkanImage),
      .image =
          reinterpret_cast<uint64_t>(backend->images_[backend->current_image_]),
      .format = static_cast<uint32_t>(backend->image_format_),
  };
}

bool HeadlessVulkanBackend::PresentCallback(
    void* user_data,
    const FlutterVulkanImage* /* image */) {
  // WS-1 does nothing with the composited frame — the pacer drives cadence.
  // Advance the round-robin index so the next frame targets a different image.
  auto* backend = BackendOf(user_data);
  backend->current_image_ = (backend->current_image_ + 1) % kImageCount;
  return true;
}

// ── Vsync ────────────────────────────────────────────────────────────────────

VsyncCallback HeadlessVulkanBackend::GetVsyncCallback() const {
  return vsync_period_ns_ != 0 ? &VsyncTrampoline : nullptr;
}

void HeadlessVulkanBackend::VsyncTrampoline(void* user_data,
                                            const intptr_t baton) {
  // user_data is the FlutterDesktopEngineState* handed to the engine. Recover
  // the engine + backend; if either is gone (shutdown race) drop the baton.
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return;
  }
  auto* engine_obj = state->view_controller->engine;
  auto* backend =
      dynamic_cast<HeadlessVulkanBackend*>(engine_obj->GetBackend());
  if (backend == nullptr || backend->pacer_ == nullptr) {
    return;
  }
  backend->pacer_->SubmitBaton(engine_obj->GetFlutterEngine(), baton);
}

void HeadlessVulkanBackend::SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine)
                                                engine) {
  engine_handle_ = engine;
  StartVsyncIfReady();
}

void HeadlessVulkanBackend::SetPlatformTaskRunner(TaskRunner* runner) {
  platform_task_runner_ = runner;
  StartVsyncIfReady();
}

void HeadlessVulkanBackend::StartVsyncIfReady() {
  if (vsync_period_ns_ == 0 || vsync_running_.load() ||
      engine_handle_ == nullptr || platform_task_runner_ == nullptr ||
      platform_task_runner_->GetIoContext() == nullptr) {
    return;
  }
  vsync_running_.store(true, std::memory_order_release);
  // Free-run (ceiling-only) pacing: WS-1 has no consumer to gate on, so the
  // pacer delivers a baton on the vsync_period_ns_ ceiling alone. The pipeline
  // depth matches the render-target ring.
  pacer_ = std::make_unique<ivi::ConsumerPacedVsyncSource>(
      vsync_, vsync_period_ns_, kImageCount);
  pacer_->Start(engine_handle_, platform_task_runner_);
  pacer_->SetFreeRun(true);
  ihs::log::info("[HeadlessVulkan] synthetic vsync at {} fps (free-run)",
                 1'000'000'000u / vsync_period_ns_);
}

void HeadlessVulkanBackend::StopVsyncMonitor() {
  if (vsync_running_.exchange(false, std::memory_order_acq_rel) &&
      pacer_ != nullptr) {
    pacer_->Stop();
  }
  vsync_.Stop();
}

// ── Trivial Backend stubs ────────────────────────────────────────────────────

void HeadlessVulkanBackend::Resize(size_t /*index*/,
                                   Engine* /*flutter_engine*/,
                                   int32_t width,
                                   int32_t height) {
  const auto w = static_cast<uint32_t>(width);
  const auto h = static_cast<uint32_t>(height);
  if (w == width_ && h == height_) {
    return;
  }
  // Geometry changes are not supported in WS-1 (the render-target ring would
  // need re-allocation). Log and keep the initial size.
  ihs::log::warn("[HeadlessVulkan] resize to {}x{} ignored (fixed at {}x{})", w,
                 h, width_, height_);
}

void HeadlessVulkanBackend::CreateSurface(size_t /*index*/,
                                          wl_surface* /*surface*/,
                                          int32_t /*width*/,
                                          int32_t /*height*/) {
  // No display surface: the engine renders into our own VkImage ring.
}

bool HeadlessVulkanBackend::TextureMakeCurrent() {
  return true;
}

bool HeadlessVulkanBackend::TextureClearCurrent() {
  return true;
}
