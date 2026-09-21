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

#include <algorithm>
#include <array>
#include <set>
#include <string>

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

// Bytes per pixel for the packed 32-bit formats this backend scans out. The
// imported path states the row pitch itself, so it has to know the stride; the
// exported path never asks, because the driver reports the layout it chose.
uint32_t BytesPerPixel(const VkFormat f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
      return 4;
    default:
      return 0;
  }
}

constexpr uint64_t AlignUp(const uint64_t v, const uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}

// Whether an image with this format, usage and DRM modifier can be created and
// exported as a dma-buf.
//
// vkCreateImage is not a substitute for this question. A driver may accept
// usage the format properties do not support -- V3D accepts SAMPLED on a
// modifier whose features lack SAMPLED_IMAGE -- and hands back a valid handle,
// so a create-and-fall-back scheme never falls back. The mismatch surfaces
// later instead: an unsampleable view at draw, and a handle type the export
// cannot honor at bind. Ask the question that has an authoritative answer.
bool SupportsUsageForExport(VkPhysicalDevice physical_device,
                            VkFormat vk_format,
                            VkImageUsageFlags usage,
                            uint64_t modifier) {
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{};
  mod_info.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
  mod_info.drmFormatModifier = modifier;
  mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkPhysicalDeviceExternalImageFormatInfo ext_info{};
  ext_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  ext_info.pNext = &mod_info;
  ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkPhysicalDeviceImageFormatInfo2 fi{};
  fi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  fi.pNext = &ext_info;
  fi.format = vk_format;
  fi.type = VK_IMAGE_TYPE_2D;
  fi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  fi.usage = usage;

  VkExternalImageFormatProperties ext_props{};
  ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  props.pNext = &ext_props;

  if (d().vkGetPhysicalDeviceImageFormatProperties2(physical_device, &fi,
                                                    &props) != VK_SUCCESS) {
    return false;
  }
  return (ext_props.externalMemoryProperties.compatibleHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
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

  // The compositor samples every store that is not the base one, so a store
  // without SAMPLED is read by a draw its usage never permitted -- two
  // validation errors per frame, and undefined behavior whatever the driver
  // does about it (#617).
  //
  // The bit cannot simply be added. The driver picks from the modifier list
  // below, which is what the scanout plane accepts, and a modifier that scans
  // out is not necessarily one that samples. Narrow the list to the candidates
  // that do, so whichever the driver picks is sampleable; where that leaves
  // nothing, keep scanout and mark the store honestly unsampleable.
  constexpr VkImageUsageFlags kBaseUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  std::vector<uint64_t> sampled_modifiers;
  for (uint64_t m : allowed_modifiers) {
    if (SupportsUsageForExport(physical_device, vk_format,
                               kBaseUsage | VK_IMAGE_USAGE_SAMPLED_BIT, m)) {
      sampled_modifiers.push_back(m);
    }
  }
  store->sampleable_ = !sampled_modifiers.empty();
  const std::vector<uint64_t>& candidates =
      store->sampleable_ ? sampled_modifiers : allowed_modifiers;

  // Exported image constrained to the negotiated modifier set; the driver picks
  // one, read back below.
  VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
  mod_list.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  mod_list.drmFormatModifierCount = static_cast<uint32_t>(candidates.size());
  mod_list.pDrmFormatModifiers = candidates.data();
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
      store->sampleable_ ? kBaseUsage | VK_IMAGE_USAGE_SAMPLED_BIT : kBaseUsage;
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
  // Dedicated, like the import path below. This allocation backs exactly one
  // image, so it costs nothing to say so -- and a driver that reports
  // requiresDedicatedAllocation for a DRM-modifier scanout image (V3D does)
  // otherwise gets memory it asked to be dedicated and was not. Validation
  // catches it at bind, five times a run; the driver does not complain, which
  // is why it sat here.
  VkMemoryDedicatedAllocateInfo ded{};
  ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  ded.pNext = &emai;
  ded.image = store->image_;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &ded;
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

std::unique_ptr<VulkanBackingStore> VulkanBackingStore::CreateImported(
    VkPhysicalDevice physical_device,
    VkDevice device,
    uint32_t width,
    uint32_t height,
    VkFormat vk_format,
    uint32_t drm_fourcc,
    const ContiguousAllocator& allocator,
    std::string& err) {
  const uint32_t bpp = BytesPerPixel(vk_format);
  if (bpp == 0) {
    err = "imported scanout: unsupported format for an explicit linear layout";
    return nullptr;
  }
  // Ask before allocating. A contiguous buffer is a scarce resource on the
  // boards that need this path, and an ICD that cannot make a linear modifier
  // image out of an imported dma-buf should say so before one is reserved
  // rather than after.
  {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{};
    mod_info.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    mod_info.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
    mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo ext_info{};
    ext_info.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    ext_info.pNext = &mod_info;
    ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 fmt_info{};
    fmt_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    fmt_info.pNext = &ext_info;
    fmt_info.format = vk_format;
    fmt_info.type = VK_IMAGE_TYPE_2D;
    fmt_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    fmt_info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties ext_props{};
    ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = &ext_props;
    if (d().vkGetPhysicalDeviceImageFormatProperties2(
            physical_device, &fmt_info, &props) != VK_SUCCESS) {
      err =
          "imported scanout: the GPU cannot make a linear modifier image from "
          "an imported dma-buf for this format";
      return nullptr;
    }
    if ((ext_props.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
      err = "imported scanout: the GPU does not support importing dma-bufs";
      return nullptr;
    }
  }
  // 64 bytes is the widest row alignment the scanout engines in this class
  // ask for, and costs at most 63 bytes a row.
  static constexpr uint64_t kRowAlign = 64;
  static constexpr uint64_t kPage = 4096;
  const uint64_t pitch = AlignUp(static_cast<uint64_t>(width) * bpp, kRowAlign);
  const uint64_t used = pitch * height;

  std::unique_ptr<VulkanBackingStore> store(
      new VulkanBackingStore(device, width, height));
  store->vk_format_ = vk_format;
  store->drm_fourcc_ = drm_fourcc;
  store->modifier_ = DRM_FORMAT_MOD_LINEAR;
  store->planes_.push_back({0, pitch});

  VkSubresourceLayout plane_layout{};
  plane_layout.offset = 0;
  plane_layout.rowPitch = pitch;
  plane_layout.size = used;
  VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod{};
  explicit_mod.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  explicit_mod.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
  explicit_mod.drmFormatModifierPlaneCount = 1;
  explicit_mod.pPlaneLayouts = &plane_layout;
  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.pNext = &explicit_mod;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

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
  // SAMPLED for the same reason as the exported path above -- this store is
  // composited like any other. The modifier is fixed by the buffer being
  // imported, so there is one candidate to ask about rather than a list.
  constexpr VkImageUsageFlags kBaseUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  store->sampleable_ = SupportsUsageForExport(
      physical_device, vk_format, kBaseUsage | VK_IMAGE_USAGE_SAMPLED_BIT,
      DRM_FORMAT_MOD_LINEAR);
  ic.usage =
      store->sampleable_ ? kBaseUsage | VK_IMAGE_USAGE_SAMPLED_BIT : kBaseUsage;
  ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (d().vkCreateImage(device, &ic, nullptr, &store->image_) != VK_SUCCESS) {
    err =
        "imported scanout: vkCreateImage with an explicit linear layout "
        "failed";
    return nullptr;
  }

  // Size the buffer only once the driver has said what the image needs, so the
  // allocation satisfies that and not merely the framebuffer arithmetic.
  //
  // Then one page beyond it. An importing driver may add a read-ahead allowance
  // to the size it demands of a foreign buffer and refuse anything smaller --
  // Mesa's v3dv does, by its TFU read-ahead constant -- so a buffer sized
  // exactly to the image gets rejected for being too small. A whole page covers
  // any such allowance without encoding one driver's constant, and KMS does not
  // care that the buffer outlasts the framebuffer.
  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device, store->image_, &req);
  const uint64_t size =
      AlignUp(std::max<uint64_t>(req.size, used), kPage) + kPage;
  store->dma_buf_fd_ = allocator.Allocate(size, err);
  if (store->dma_buf_fd_ < 0) {
    return nullptr;  // the destructor ignores a negative fd
  }

  // Which memory types can back this fd, intersected with what the image
  // accepts. The fd decides the heap, so DEVICE_LOCAL is not a usable filter
  // here the way it is for an allocation we own.
  VkMemoryFdPropertiesKHR fdp{};
  fdp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
  if (d().vkGetMemoryFdPropertiesKHR(
          device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          store->dma_buf_fd_, &fdp) != VK_SUCCESS) {
    err = "imported scanout: vkGetMemoryFdPropertiesKHR failed";
    return nullptr;
  }
  const uint32_t usable = req.memoryTypeBits & fdp.memoryTypeBits;
  if (usable == 0) {
    err =
        "imported scanout: no memory type common to the image and the "
        "dma-buf";
    return nullptr;
  }
  uint32_t mt = 0;
  while ((usable & (1u << mt)) == 0) {
    ++mt;
  }

  // vkAllocateMemory consumes the fd it is given, so hand it a duplicate and
  // keep ours: the framebuffer import needs the same buffer afterwards.
  const int import_fd = ::dup(store->dma_buf_fd_);
  if (import_fd < 0) {
    err = "imported scanout: dup of the dma-buf fd failed";
    return nullptr;
  }
  VkImportMemoryFdInfoKHR imp{};
  imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  imp.fd = import_fd;
  VkMemoryDedicatedAllocateInfo ded{};
  ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  ded.pNext = &imp;
  ded.image = store->image_;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &ded;
  // The driver applies its own rounding to this; passing the padded size would
  // round it a second time and ask for more than was allocated.
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (d().vkAllocateMemory(device, &mai, nullptr, &store->memory_) !=
      VK_SUCCESS) {
    ::close(import_fd);
    err = "imported scanout: vkAllocateMemory (import) failed";
    return nullptr;
  }
  if (d().vkBindImageMemory(device, store->image_, store->memory_, 0) !=
      VK_SUCCESS) {
    err = "imported scanout: vkBindImageMemory failed";
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
    err = "imported scanout: vkCreateImageView failed";
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
