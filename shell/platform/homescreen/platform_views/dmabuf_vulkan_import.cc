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

#include <cstdint>

#include "logging/logging.h"

namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// The compositor samples platform-view images through a B8G8R8A8 view, so map
// the RGB fourccs to the matching Vulkan format. Alpha vs. no-alpha share a
// format (X* just ignores alpha). YUV maps land with the video path.
VkFormat VkFormatFromFourcc(uint32_t fourcc) {
  if (fourcc == Fourcc('A', 'R', '2', '4') ||  // DRM_FORMAT_ARGB8888
      fourcc == Fourcc('X', 'R', '2', '4')) {  // DRM_FORMAT_XRGB8888
    return VK_FORMAT_B8G8R8A8_UNORM;
  }
  if (fourcc == Fourcc('A', 'B', '2', '4') ||  // DRM_FORMAT_ABGR8888
      fourcc == Fourcc('X', 'B', '2', '4')) {  // DRM_FORMAT_XBGR8888
    return VK_FORMAT_R8G8B8A8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
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
  if (!ready() || out == nullptr || frame.plane_count != 1 ||
      frame.plane_fd[0] < 0) {
    return false;
  }
  const VkFormat format = VkFormatFromFourcc(frame.format.fourcc);
  if (format == VK_FORMAT_UNDEFINED) {
    ihs::log::warn("[ihs_pv] dma-buf import: unsupported fourcc {:#x}",
                   frame.format.fourcc);
    return false;
  }

  // Explicit modifier + plane layout the producer exported with, so the driver
  // reads the tiled memory correctly.
  VkSubresourceLayout plane_layout{};
  plane_layout.offset = frame.plane_offset[0];
  plane_layout.rowPitch = frame.plane_stride[0];
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{};
  mod.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  mod.drmFormatModifier = frame.format.modifier;
  mod.drmFormatModifierPlaneCount = 1;
  mod.pPlaneLayouts = &plane_layout;

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
