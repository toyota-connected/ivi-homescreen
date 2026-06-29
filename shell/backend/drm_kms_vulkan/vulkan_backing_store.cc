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

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "vulkan_backing_store.h"

#include <drm_fourcc.h>
#include <unistd.h>

#include <array>
#include <set>

namespace drm_kms_vulkan {

namespace {

// The dynamic dispatcher; storage is defined in device_caps.cc. It is init'd to
// the instance and device by the backend before any backing store is created,
// so the device-level modifier/external-memory entry points are resolved here.
// An accessor (not a namespace-scope reference) so there is no
// static-init-order dependency on that other TU's global.
const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

uint32_t PickMemoryType(VkPhysicalDevice phys,
                        uint32_t type_bits,
                        VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  d().vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & want) == want) {
      return i;
    }
  }
  return UINT32_MAX;
}

// Plane count the driver uses for @p modifier with @p vk_format.
uint32_t PlaneCountForModifier(VkPhysicalDevice phys,
                               VkFormat vk_format,
                               uint64_t modifier) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  d().vkGetPhysicalDeviceFormatProperties2(phys, vk_format, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  d().vkGetPhysicalDeviceFormatProperties2(phys, vk_format, &fp);
  for (const auto& m : mods) {
    if (m.drmFormatModifier == modifier) {
      return m.drmFormatModifierPlaneCount;
    }
  }
  return 0;
}

}  // namespace

std::vector<uint64_t> NegotiateModifiers(
    VkPhysicalDevice physical_device,
    VkFormat vk_format,
    const std::vector<uint64_t>& plane_modifiers) {
  // Modifiers the ICD can export for this format as a transfer/color target.
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  d().vkGetPhysicalDeviceFormatProperties2(physical_device, vk_format, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  d().vkGetPhysicalDeviceFormatProperties2(physical_device, vk_format, &fp);

  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  std::set<uint64_t> exportable;
  for (const auto& m : mods) {
    if ((m.drmFormatModifierTilingFeatures & need) == need) {
      exportable.insert(m.drmFormatModifier);
    }
  }

  const std::set<uint64_t> plane(plane_modifiers.begin(),
                                 plane_modifiers.end());
  std::vector<uint64_t> tiled;
  bool has_linear = false;
  for (uint64_t m : exportable) {
    if (!plane.count(m)) {
      continue;
    }
    if (m == DRM_FORMAT_MOD_LINEAR) {
      has_linear = true;
    } else {
      tiled.push_back(m);
    }
  }
  // Tiled first (the driver prefers a compressed/tiled layout), LINEAR last.
  if (has_linear) {
    tiled.push_back(DRM_FORMAT_MOD_LINEAR);
  }
  return tiled;
}

std::unique_ptr<VulkanBackingStore> VulkanBackingStore::Create(
    VkPhysicalDevice physical_device,
    VkDevice device,
    uint32_t width,
    uint32_t height,
    VkFormat vk_format,
    uint32_t drm_fourcc,
    const std::vector<uint64_t>& allowed_modifiers,
    std::string& err) {
  if (allowed_modifiers.empty()) {
    err = "no allowed modifiers (empty negotiation result)";
    return nullptr;
  }

  std::unique_ptr<VulkanBackingStore> store(
      new VulkanBackingStore(device, width, height));
  store->vk_format_ = vk_format;
  store->drm_fourcc_ = drm_fourcc;

  // Exported image constrained to the negotiated modifier set; the driver picks
  // one, read back below.
  VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
  mod_list.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  mod_list.drmFormatModifierCount =
      static_cast<uint32_t>(allowed_modifiers.size());
  mod_list.pDrmFormatModifiers = allowed_modifiers.data();
  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ext.pNext = &mod_list;

  VkImageCreateInfo ic{};
  ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ic.pNext = &ext;
  ic.imageType = VK_IMAGE_TYPE_2D;
  ic.format = vk_format;
  ic.extent = {width, height, 1};
  ic.mipLevels = 1;
  ic.arrayLayers = 1;
  ic.samples = VK_SAMPLE_COUNT_1_BIT;
  ic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ic.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (d().vkCreateImage(device, &ic, nullptr, &store->image_) != VK_SUCCESS) {
    err = "vkCreateImage with modifier list failed";
    return nullptr;
  }

  VkImageDrmFormatModifierPropertiesEXT mp{};
  mp.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
  if (d().vkGetImageDrmFormatModifierPropertiesEXT(device, store->image_,
                                                   &mp) != VK_SUCCESS) {
    err = "vkGetImageDrmFormatModifierPropertiesEXT failed";
    return nullptr;
  }
  store->modifier_ = mp.drmFormatModifier;

  const uint32_t plane_count =
      PlaneCountForModifier(physical_device, vk_format, store->modifier_);
  if (plane_count < 1 || plane_count > 4) {
    err = "unexpected plane count for chosen modifier";
    return nullptr;
  }

  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device, store->image_, &req);
  const uint32_t mt = PickMemoryType(physical_device, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt == UINT32_MAX) {
    err = "no DEVICE_LOCAL memory type for scanout image";
    return nullptr;
  }
  VkExportMemoryAllocateInfo emai{};
  emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &emai;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (d().vkAllocateMemory(device, &mai, nullptr, &store->memory_) !=
      VK_SUCCESS) {
    err = "vkAllocateMemory (exported) failed";
    return nullptr;
  }
  if (d().vkBindImageMemory(device, store->image_, store->memory_, 0) !=
      VK_SUCCESS) {
    err = "vkBindImageMemory failed";
    return nullptr;
  }

  static constexpr std::array<VkImageAspectFlagBits, 4> kMemPlane = {
      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
  };
  store->planes_.resize(plane_count);
  for (uint32_t i = 0; i < plane_count; ++i) {
    VkImageSubresource sub{};
    sub.aspectMask = kMemPlane[i];
    VkSubresourceLayout sl{};
    d().vkGetImageSubresourceLayout(device, store->image_, &sub, &sl);
    store->planes_[i] = {sl.offset, sl.rowPitch};
  }

  VkMemoryGetFdInfoKHR gfi{};
  gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  gfi.memory = store->memory_;
  gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if (d().vkGetMemoryFdKHR(device, &gfi, &store->dma_buf_fd_) != VK_SUCCESS) {
    err = "vkGetMemoryFdKHR failed";
    return nullptr;
  }

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = store->image_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = vk_format;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (d().vkCreateImageView(device, &vci, nullptr, &store->view_) !=
      VK_SUCCESS) {
    err = "vkCreateImageView failed";
    return nullptr;
  }

  store->flutter_image_.struct_size = sizeof(FlutterVulkanImage);
  store->flutter_image_.image = reinterpret_cast<uint64_t>(store->image_);
  store->flutter_image_.format = static_cast<uint32_t>(vk_format);
  return store;
}

VulkanBackingStore::VulkanBackingStore(VkDevice device,
                                       uint32_t width,
                                       uint32_t height)
    : device_(device), width_(width), height_(height) {}

VulkanBackingStore::~VulkanBackingStore() {
  if (view_ != VK_NULL_HANDLE) {
    d().vkDestroyImageView(device_, view_, nullptr);
  }
  if (image_ != VK_NULL_HANDLE) {
    d().vkDestroyImage(device_, image_, nullptr);
  }
  if (memory_ != VK_NULL_HANDLE) {
    d().vkFreeMemory(device_, memory_, nullptr);
  }
  if (dma_buf_fd_ >= 0) {
    ::close(dma_buf_fd_);
  }
}

}  // namespace drm_kms_vulkan
