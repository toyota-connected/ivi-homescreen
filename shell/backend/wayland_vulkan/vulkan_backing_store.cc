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

#include "backend/wayland_vulkan/vulkan_backing_store.h"
#include "logging/logging.h"

#include <unistd.h>

#include <drm_fourcc.h>  // DRM_FORMAT_MOD_LINEAR

#include <array>
#include <optional>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "logging.h"

namespace {

const auto& Dispatch() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

std::optional<uint32_t> FindMemoryType(VkPhysicalDevice gpu,
                                       uint32_t type_bits,
                                       VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties props{};
  Dispatch().vkGetPhysicalDeviceMemoryProperties(gpu, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((type_bits & (1U << i)) &&
        (props.memoryTypes[i].propertyFlags & required) == required) {
      return i;
    }
  }
  return std::nullopt;
}

// Can an image created with exactly these parameters export a dma-buf?
//
// This is the query VUID-VkImageCreateInfo-pNext-00990 and
// VUID-VkExportMemoryAllocateInfo-handleTypes-09860 are written against:
// compatibleHandleTypes is a property of the (format, type, tiling, usage,
// flags) tuple, not of the device, so asking for a dma-buf export without
// asking first is how an image ends up created with an export the driver never
// promised. On Mesa the answer for VK_IMAGE_TILING_OPTIMAL is
// VK_ERROR_FORMAT_NOT_SUPPORTED.
bool DmaBufExportable(VkPhysicalDevice gpu,
                      VkImageTiling tiling,
                      std::optional<uint64_t> modifier) {
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{};
  mod_info.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
  mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkPhysicalDeviceExternalImageFormatInfo ext_info{};
  ext_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if (modifier) {
    mod_info.drmFormatModifier = *modifier;
    ext_info.pNext = &mod_info;
  }

  VkPhysicalDeviceImageFormatInfo2 info{};
  info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  info.pNext = &ext_info;
  info.format = VulkanBackingStore::kFormat;
  info.type = VK_IMAGE_TYPE_2D;
  info.tiling = tiling;
  info.usage = VulkanBackingStore::kUsage;

  VkExternalImageFormatProperties ext_props{};
  ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  props.pNext = &ext_props;

  if (Dispatch().vkGetPhysicalDeviceImageFormatProperties2(
          gpu, &info, &props) != VK_SUCCESS) {
    return false;
  }
  const auto& p = ext_props.externalMemoryProperties;
  return (p.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0 &&
         (p.compatibleHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
}

// Every DRM format modifier the driver knows for kFormat, whatever its tiling
// features; DmaBufExportable then decides which of them actually work with
// kUsage, so this deliberately does not pre-filter on the feature flags.
std::vector<uint64_t> SupportedModifiers(VkPhysicalDevice gpu) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  Dispatch().vkGetPhysicalDeviceFormatProperties2(
      gpu, VulkanBackingStore::kFormat, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  if (mods.empty()) {
    return {};
  }
  list.pDrmFormatModifierProperties = mods.data();
  Dispatch().vkGetPhysicalDeviceFormatProperties2(
      gpu, VulkanBackingStore::kFormat, &fp);

  std::vector<uint64_t> out;
  out.reserve(mods.size());
  for (const auto& m : mods) {
    out.push_back(m.drmFormatModifier);
  }
  return out;
}

// Memory planes the chosen modifier uses, so the exported layout can be read
// back plane by plane.
uint32_t PlaneCountForModifier(VkPhysicalDevice gpu, uint64_t modifier) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  Dispatch().vkGetPhysicalDeviceFormatProperties2(
      gpu, VulkanBackingStore::kFormat, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  if (mods.empty()) {
    return 0;
  }
  list.pDrmFormatModifierProperties = mods.data();
  Dispatch().vkGetPhysicalDeviceFormatProperties2(
      gpu, VulkanBackingStore::kFormat, &fp);
  for (const auto& m : mods) {
    if (m.drmFormatModifier == modifier) {
      return m.drmFormatModifierPlaneCount;
    }
  }
  return 0;
}

}  // namespace

VulkanStoreExportPlan VulkanBackingStore::PlanExport(
    VkPhysicalDevice physical_device,
    const bool have_drm_format_modifier_ext) {
  VulkanStoreExportPlan plan;
  const auto& d = Dispatch();
  if (physical_device == VK_NULL_HANDLE ||
      d.vkGetPhysicalDeviceImageFormatProperties2 == nullptr) {
    return plan;
  }

  // Opaque tiling first: where a driver does report it exportable there is no
  // modifier to track and no layout to read back, so the simpler path stays.
  if (DmaBufExportable(physical_device, VK_IMAGE_TILING_OPTIMAL,
                       std::nullopt)) {
    plan.enabled = true;
    plan.tiling = VK_IMAGE_TILING_OPTIMAL;
    return plan;
  }

  if (!have_drm_format_modifier_ext ||
      d.vkGetPhysicalDeviceFormatProperties2 == nullptr) {
    return plan;
  }

  // LINEAR last, as the sibling present path orders it. The driver chooses
  // among the candidates by its own rule, so this is a hint at most -- what
  // matters is that a tiled modifier is in the list at all, since the engine
  // renders into this image every frame.
  std::vector<uint64_t> candidates;
  bool linear = false;
  for (const uint64_t m : SupportedModifiers(physical_device)) {
    if (!DmaBufExportable(physical_device,
                          VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, m)) {
      continue;
    }
    if (m == DRM_FORMAT_MOD_LINEAR) {
      linear = true;
    } else {
      candidates.push_back(m);
    }
  }
  if (linear) {
    candidates.push_back(DRM_FORMAT_MOD_LINEAR);
  }
  if (candidates.empty()) {
    return plan;
  }

  plan.enabled = true;
  plan.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  plan.modifiers = std::move(candidates);
  return plan;
}

// A constructor cannot report failure and this one does not throw, so every
// step below follows the same shape: log, Destroy() what was built so far, and
// return. The object is left with image_ == VK_NULL_HANDLE, which is what
// IsValid() reports -- so the caller checks IsValid() after constructing, and a
// store that failed here is inert rather than half-built.
VulkanBackingStore::VulkanBackingStore(int32_t width,
                                       int32_t height,
                                       VkDevice device,
                                       VkPhysicalDevice physical_device,
                                       const VulkanStoreExportPlan& export_plan)
    : width_(width), height_(height), device_(device) {
  const auto& d = Dispatch();
  bool export_dma_buf = export_plan.enabled;
  const bool modifier_tiling =
      export_dma_buf &&
      export_plan.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

  // The driver picks one of these; which one it picked is read back after
  // creation, because an importer cannot be told the layout otherwise.
  VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
  mod_list.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  mod_list.drmFormatModifierCount =
      static_cast<uint32_t>(export_plan.modifiers.size());
  mod_list.pDrmFormatModifiers = export_plan.modifiers.data();

  VkExternalMemoryImageCreateInfo ext_image_info{};
  ext_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if (modifier_tiling) {
    ext_image_info.pNext = &mod_list;
  }

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kFormat;
  image_info.extent = {static_cast<uint32_t>(width_),
                       static_cast<uint32_t>(height_), 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  // The tiling the plan was queried with, not an assumed one: on most drivers
  // a dma-buf export is only compatible with DRM_FORMAT_MODIFIER tiling, and
  // the export structures below are only in spec for the tiling the query
  // answered for.
  image_info.tiling =
      export_dma_buf ? export_plan.tiling : VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = kUsage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Both the image and its memory have to be told about the export, and they
  // have to agree: an image created without VkExternalMemoryImageCreateInfo
  // cannot be bound to exportable memory. Hence the same flag gates the pNext
  // here and on the allocation below.
  if (export_dma_buf) {
    image_info.pNext = &ext_image_info;
  }

  if (d.vkCreateImage(device_, &image_info, nullptr, &image_) != VK_SUCCESS) {
    ihs::log::error("VulkanBackingStore: vkCreateImage failed ({}x{})", width_,
                    height_);
    return;
  }

  // vkGetImageMemoryRequirements2 (core 1.1) rather than the original call,
  // for the dedicated-allocation requirement that comes with it: an
  // exportable image usually reports requiresDedicatedAllocation, and binding
  // memory that was not allocated for this one image is then out of spec
  // (VUID-vkBindImageMemory-image-01445).
  VkMemoryDedicatedRequirements dedicated_req{};
  dedicated_req.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 req2{};
  req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  req2.pNext = &dedicated_req;
  VkMemoryRequirements req{};
  if (d.vkGetImageMemoryRequirements2 != nullptr) {
    VkImageMemoryRequirementsInfo2 req_info{};
    req_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    req_info.image = image_;
    d.vkGetImageMemoryRequirements2(device_, &req_info, &req2);
    req = req2.memoryRequirements;
  } else {
    d.vkGetImageMemoryRequirements(device_, image_, &req);
  }

  auto type_index = FindMemoryType(physical_device, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!type_index) {
    ihs::log::error("VulkanBackingStore: no device-local memory type");
    Destroy();
    return;
  }

  VkMemoryDedicatedAllocateInfo dedicated_info{};
  dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated_info.image = image_;
  const bool dedicated = dedicated_req.requiresDedicatedAllocation != 0U ||
                         dedicated_req.prefersDedicatedAllocation != 0U;

  VkExportMemoryAllocateInfo export_info{};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if (dedicated) {
    export_info.pNext = &dedicated_info;
  }

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = req.size;
  alloc_info.memoryTypeIndex = *type_index;
  if (export_dma_buf) {
    alloc_info.pNext = &export_info;
  } else if (dedicated) {
    alloc_info.pNext = &dedicated_info;
  }

  VkResult alloc_res =
      d.vkAllocateMemory(device_, &alloc_info, nullptr, &memory_);
  if (alloc_res != VK_SUCCESS && export_dma_buf) {
    // Retry without export on drivers that advertise the extension but
    // refuse DMA-BUF-exportable allocations for this format.
    ihs::log::warn(
        "VulkanBackingStore: DMA-BUF export alloc failed ({}); falling back",
        static_cast<int>(alloc_res));
    // Only the export is dropped. The image keeps whatever tiling it was
    // created with -- modifier tiling binds plain memory perfectly well, and
    // rebuilding the image to get the opaque one back would buy nothing but a
    // second failure path.
    alloc_info.pNext = dedicated ? &dedicated_info : nullptr;
    alloc_res = d.vkAllocateMemory(device_, &alloc_info, nullptr, &memory_);
    export_dma_buf = false;
  }
  if (alloc_res != VK_SUCCESS) {
    ihs::log::error("VulkanBackingStore: vkAllocateMemory failed ({})",
                    static_cast<int>(alloc_res));
    Destroy();
    return;
  }

  if (d.vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
    ihs::log::error("VulkanBackingStore: vkBindImageMemory failed");
    Destroy();
    return;
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = kFormat;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  if (d.vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
    ihs::log::error("VulkanBackingStore: vkCreateImageView failed");
    Destroy();
    return;
  }

  // The second of two independent ways export can end up unavailable. The
  // first is the allocation retry above, which clears export_dma_buf and skips
  // this block entirely. This one happens after a *successful* exportable
  // allocation: the driver may not expose vkGetMemoryFdKHR, or the call may
  // fail. Either way dma_buf_fd_ stays -1 and the store remains fully valid for
  // rendering -- only has_dma_buf() goes false, which is why callers test that
  // rather than inferring it from the export_dma_buf they asked for.
  if (export_dma_buf) {
    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory_;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (d.vkGetMemoryFdKHR) {
      VkResult r = d.vkGetMemoryFdKHR(device_, &fd_info, &dma_buf_fd_);
      if (r != VK_SUCCESS) {
        ihs::log::warn("VulkanBackingStore: vkGetMemoryFdKHR failed ({})",
                       static_cast<int>(r));
        dma_buf_fd_ = -1;
      }
    }
  }

  // An fd on its own does not describe a buffer. On the modifier path, read
  // back which modifier the driver chose and where each of its memory planes
  // starts, so an importer has the whole description. The opaque path has
  // nothing to report: there is no modifier to name and no defined layout,
  // which is why it is only taken when the driver says it is exportable.
  if (dma_buf_fd_ >= 0 && modifier_tiling) {
    VkImageDrmFormatModifierPropertiesEXT mod_props{};
    mod_props.sType =
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (d.vkGetImageDrmFormatModifierPropertiesEXT != nullptr &&
        d.vkGetImageDrmFormatModifierPropertiesEXT(device_, image_,
                                                   &mod_props) == VK_SUCCESS) {
      dma_buf_modifier_ = mod_props.drmFormatModifier;

      static constexpr std::array<VkImageAspectFlagBits, 4> kMemoryPlane = {
          VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
          VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
          VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
          VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
      };
      const uint32_t planes =
          PlaneCountForModifier(physical_device, *dma_buf_modifier_);
      for (uint32_t i = 0; i < planes && i < kMemoryPlane.size(); ++i) {
        VkImageSubresource sub{};
        sub.aspectMask = kMemoryPlane[i];
        VkSubresourceLayout layout{};
        d.vkGetImageSubresourceLayout(device_, image_, &sub, &layout);
        dma_buf_planes_.push_back({layout.offset, layout.rowPitch});
      }
    } else {
      // Without the modifier the fd cannot be described, so do not hand out a
      // buffer nobody can interpret.
      ihs::log::warn(
          "VulkanBackingStore: vkGetImageDrmFormatModifierPropertiesEXT "
          "failed; dropping the export");
      ::close(dma_buf_fd_);
      dma_buf_fd_ = -1;
    }
  }

  // Published last, so it is only populated on the path where everything
  // above succeeded. The engine takes the VkImage as an opaque uint64 handle.
  engine_image_ = {
      .struct_size = sizeof(FlutterVulkanImage),
      .image = reinterpret_cast<uint64_t>(image_),
      .format = static_cast<uint32_t>(kFormat),
  };
}

VulkanBackingStore::~VulkanBackingStore() {
  Destroy();
}

void VulkanBackingStore::Destroy() {
  const auto& d = Dispatch();
  if (dma_buf_fd_ >= 0) {
    ::close(dma_buf_fd_);
    dma_buf_fd_ = -1;
  }
  dma_buf_modifier_.reset();
  dma_buf_planes_.clear();
  if (device_) {
    if (view_) {
      d.vkDestroyImageView(device_, view_, nullptr);
      view_ = VK_NULL_HANDLE;
    }
    if (image_) {
      d.vkDestroyImage(device_, image_, nullptr);
      image_ = VK_NULL_HANDLE;
    }
    if (memory_) {
      d.vkFreeMemory(device_, memory_, nullptr);
      memory_ = VK_NULL_HANDLE;
    }
  }
}
