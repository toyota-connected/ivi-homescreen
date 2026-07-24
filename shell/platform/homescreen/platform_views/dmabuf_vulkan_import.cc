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

#include "dmabuf_vulkan_import.h"

#include <array>
#include <cstdint>

#include "logging/logging.h"

namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// Maps a DRM fourcc to the Vulkan format an imported dma-buf is created with.
// The packed RGB formats are sampled through a plain B8G8R8A8-style view; the
// planar ones through a VkSamplerYcbcrConversion the compositor builds.
// `is_yuv` distinguishes the two so the caller knows which path an image needs.
VkFormat VkFormatFromFourcc(uint32_t fourcc, bool* is_yuv) {
  *is_yuv = false;
  if (fourcc == Fourcc('A', 'R', '2', '4') ||  // DRM_FORMAT_ARGB8888
      fourcc == Fourcc('X', 'R', '2', '4')) {  // DRM_FORMAT_XRGB8888
    return VK_FORMAT_B8G8R8A8_UNORM;
  }
  if (fourcc == Fourcc('A', 'B', '2', '4') ||  // DRM_FORMAT_ABGR8888
      fourcc == Fourcc('X', 'B', '2', '4')) {  // DRM_FORMAT_XBGR8888
    return VK_FORMAT_R8G8B8A8_UNORM;
  }
  // NV12: Y plane then interleaved CbCr, 4:2:0. What the VAAPI and V4L2
  // decoders both emit. Two planes on one dma-buf, so a non-disjoint 2-plane
  // image; the plane offsets come from the frame.
  if (fourcc == Fourcc('N', 'V', '1', '2')) {  // DRM_FORMAT_NV12
    *is_yuv = true;
    return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

// The YCbCr->RGB conversion the compositor's sampler must apply, resolved from
// the frame's color metadata. IHS_COLOR_SPACE_DEFAULT follows the contract in
// platform_view.h -- BT.601 for SD, BT.709 for HD, split at the standard 720p
// boundary -- and IHS_COLOR_RANGE_DEFAULT is studio (narrow) range, what the
// hardware decoders emit. This is the first shell implementation of that
// default; the EGL path leaves the same choice to the GL driver.
void ResolveYcbcr(const IhsFrame& frame,
                  VkSamplerYcbcrModelConversion* model,
                  VkSamplerYcbcrRange* range) {
  switch (frame.color_space) {
    case IHS_COLOR_SPACE_BT601:
      *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
      break;
    case IHS_COLOR_SPACE_BT709:
      *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
      break;
    case IHS_COLOR_SPACE_BT2020:
      *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
      break;
    default:  // IHS_COLOR_SPACE_DEFAULT
      *model = frame.height >= 720
                   ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
                   : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
      break;
  }
  *range = frame.color_range == IHS_COLOR_RANGE_FULL
               ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
               : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
}

// The first memory type common to the image's requirements and the imported
// fd's properties, or UINT32_MAX. dma-buf memory need not be DEVICE_LOCAL, so
// intersect the two masks rather than demanding a property.
uint32_t PickMemoryType(const VkPhysicalDeviceMemoryProperties& props,
                        uint32_t allowed) {
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((allowed & (1u << i)) != 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

}  // namespace

bool DmabufVulkanImporter::Init(VkInstance instance,
                                VkPhysicalDevice physical_device,
                                VkDevice device,
                                void* get_instance_proc_addr) {
  if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE ||
      device == VK_NULL_HANDLE || get_instance_proc_addr == nullptr) {
    return false;
  }
  auto gipa =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
  auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      gipa(instance, "vkGetDeviceProcAddr"));
  if (get_device_proc_addr == nullptr) {
    return false;
  }

  auto instance_fn = [&](const char* name) { return gipa(instance, name); };
  auto device_fn = [&](const char* name) {
    return get_device_proc_addr(device, name);
  };

  get_memory_properties_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          instance_fn("vkGetPhysicalDeviceMemoryProperties"));
  create_image_ =
      reinterpret_cast<PFN_vkCreateImage>(device_fn("vkCreateImage"));
  destroy_image_ =
      reinterpret_cast<PFN_vkDestroyImage>(device_fn("vkDestroyImage"));
  get_image_memory_requirements_ =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
          device_fn("vkGetImageMemoryRequirements"));
  allocate_memory_ =
      reinterpret_cast<PFN_vkAllocateMemory>(device_fn("vkAllocateMemory"));
  free_memory_ = reinterpret_cast<PFN_vkFreeMemory>(device_fn("vkFreeMemory"));
  bind_image_memory_ =
      reinterpret_cast<PFN_vkBindImageMemory>(device_fn("vkBindImageMemory"));
  get_memory_fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      device_fn("vkGetMemoryFdPropertiesKHR"));

  if (get_memory_properties_ == nullptr || create_image_ == nullptr ||
      destroy_image_ == nullptr || get_image_memory_requirements_ == nullptr ||
      allocate_memory_ == nullptr || free_memory_ == nullptr ||
      bind_image_memory_ == nullptr || get_memory_fd_properties_ == nullptr) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import unavailable: the backend's Vulkan device is "
        "missing an external-memory-fd entry point");
    return false;
  }

  instance_ = instance;
  physical_device_ = physical_device;
  device_ = device;
  return true;
}

bool DmabufVulkanImporter::Import(const IhsFrame& frame,
                                  ImportedImage* out) const {
  if (!ready() || out == nullptr) {
    return false;
  }
  bool is_yuv = false;
  const VkFormat format = VkFormatFromFourcc(frame.format.fourcc, &is_yuv);
  if (format == VK_FORMAT_UNDEFINED) {
    ihs::log::warn("[ihs_pv] dma-buf import: unsupported fourcc {:#x}",
                   frame.format.fourcc);
    return false;
  }

  // The plane count is fixed by the format: 1 for the packed RGB formats, 2 for
  // NV12. Reject a frame whose plane_count disagrees so an RGB fourcc cannot
  // arrive claiming two planes (which would mis-drive the plane-layout array
  // and vkCreateImage) and NV12 cannot arrive under-specified.
  const uint32_t expected_planes = is_yuv ? 2u : 1u;
  if (frame.plane_count != expected_planes) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import: fourcc {:#x} expects {} plane(s), got {}",
        frame.format.fourcc, expected_planes, frame.plane_count);
    return false;
  }
  if (frame.plane_fd[0] < 0) {
    ihs::log::warn("[ihs_pv] dma-buf import: fourcc {:#x} has no dma-buf fd",
                   frame.format.fourcc);
    return false;
  }
  // All planes must live in the one dma-buf -- the non-disjoint layout the
  // VAAPI and V4L2 decoders emit, imported through plane_fd[0] with the offsets
  // indexing into it. The IhsFrame contract lets "one fd back several planes",
  // so a secondary plane_fd is either unset (-1) or a duplicate handle to
  // plane_fd[0]; both are accepted. A distinct fd is the disjoint layout this
  // path does not import (it would read the wrong memory).
  for (uint32_t p = 1; p < frame.plane_count; ++p) {
    if (frame.plane_fd[p] >= 0 && frame.plane_fd[p] != frame.plane_fd[0]) {
      ihs::log::warn(
          "[ihs_pv] dma-buf import: multi-planar frame with separate per-plane "
          "fds not supported (planes must share one dma-buf)");
      return false;
    }
  }

  // One explicit layout per plane, all within the single dma-buf's allocation:
  // the driver reads Y and interleaved CbCr from their offsets in the same
  // memory (a non-disjoint multi-planar image).
  std::array<VkSubresourceLayout, 2> plane_layouts{};
  for (uint32_t p = 0; p < frame.plane_count && p < 2; ++p) {
    plane_layouts[p].offset = frame.plane_offset[p];
    plane_layouts[p].rowPitch = frame.plane_stride[p];
  }
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{};
  mod.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  mod.drmFormatModifier = frame.format.modifier;
  mod.drmFormatModifierPlaneCount = frame.plane_count;
  mod.pPlaneLayouts = plane_layouts.data();

  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ext.pNext = &mod;

  VkImageCreateInfo ic{};
  ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ic.pNext = &ext;
  ic.imageType = VK_IMAGE_TYPE_2D;
  ic.format = format;
  ic.extent = {frame.width, frame.height, 1};
  ic.mipLevels = 1;
  ic.arrayLayers = 1;
  ic.samples = VK_SAMPLE_COUNT_1_BIT;
  ic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  if (create_image_(device_, &ic, nullptr, &image) != VK_SUCCESS) {
    ihs::log::warn("[ihs_pv] dma-buf import: vkCreateImage failed");
    return false;
  }

  VkMemoryRequirements req{};
  get_image_memory_requirements_(device_, image, &req);

  VkMemoryFdPropertiesKHR fd_props{};
  fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
  if (get_memory_fd_properties_(device_,
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                frame.plane_fd[0], &fd_props) != VK_SUCCESS) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import: vkGetMemoryFdPropertiesKHR failed");
    destroy_image_(device_, image, nullptr);
    return false;
  }

  VkPhysicalDeviceMemoryProperties mem_props{};
  get_memory_properties_(physical_device_, &mem_props);
  const uint32_t mt =
      PickMemoryType(mem_props, req.memoryTypeBits & fd_props.memoryTypeBits);
  if (mt == UINT32_MAX) {
    ihs::log::warn("[ihs_pv] dma-buf import: no compatible memory type");
    destroy_image_(device_, image, nullptr);
    return false;
  }

  // Dedicated allocation is required for a dma-buf-backed image on many
  // drivers.
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = image;
  VkImportMemoryFdInfoKHR import_fd{};
  import_fd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import_fd.pNext = &dedicated;
  import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import_fd.fd = frame.plane_fd[0];
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &import_fd;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  // On success Vulkan owns the imported fd; on failure ownership stays with the
  // caller, so leave it untouched here.
  if (allocate_memory_(device_, &mai, nullptr, &memory) != VK_SUCCESS) {
    ihs::log::warn("[ihs_pv] dma-buf import: vkAllocateMemory (import) failed");
    destroy_image_(device_, image, nullptr);
    return false;
  }
  if (bind_image_memory_(device_, image, memory, 0) != VK_SUCCESS) {
    ihs::log::warn("[ihs_pv] dma-buf import: vkBindImageMemory failed");
    free_memory_(device_, memory, nullptr);  // closes the imported fd
    destroy_image_(device_, image, nullptr);
    return false;
  }

  out->image = image;
  out->memory = memory;
  out->width = frame.width;
  out->height = frame.height;
  out->format = format;
  out->yuv = is_yuv;
  if (is_yuv) {
    ResolveYcbcr(frame, &out->ycbcr_model, &out->ycbcr_range);
  }
  return true;
}

void DmabufVulkanImporter::Destroy(ImportedImage* image) const {
  if (image == nullptr || !ready()) {
    return;
  }
  if (image->image != VK_NULL_HANDLE) {
    destroy_image_(device_, image->image, nullptr);
    image->image = VK_NULL_HANDLE;
  }
  if (image->memory != VK_NULL_HANDLE) {
    free_memory_(device_, image->memory, nullptr);  // closes the imported fd
    image->memory = VK_NULL_HANDLE;
  }
}
