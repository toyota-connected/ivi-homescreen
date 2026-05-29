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

}  // namespace

VulkanBackingStore::VulkanBackingStore(int32_t width,
                                       int32_t height,
                                       VkDevice device,
                                       VkPhysicalDevice physical_device,
                                       bool export_dma_buf)
    : width_(width), height_(height), device_(device) {
  const auto& d = Dispatch();

  VkExternalMemoryImageCreateInfo ext_image_info{};
  ext_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kFormat;
  image_info.extent = {static_cast<uint32_t>(width_),
                       static_cast<uint32_t>(height_), 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (export_dma_buf) {
    image_info.pNext = &ext_image_info;
  }

  if (d.vkCreateImage(device_, &image_info, nullptr, &image_) != VK_SUCCESS) {
    spdlog::error("VulkanBackingStore: vkCreateImage failed ({}x{})", width_,
                  height_);
    return;
  }

  VkMemoryRequirements req{};
  d.vkGetImageMemoryRequirements(device_, image_, &req);

  auto type_index = FindMemoryType(physical_device, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!type_index) {
    spdlog::error("VulkanBackingStore: no device-local memory type");
    Destroy();
    return;
  }

  VkExportMemoryAllocateInfo export_info{};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = req.size;
  alloc_info.memoryTypeIndex = *type_index;
  if (export_dma_buf) {
    alloc_info.pNext = &export_info;
  }

  VkResult alloc_res =
      d.vkAllocateMemory(device_, &alloc_info, nullptr, &memory_);
  if (alloc_res != VK_SUCCESS && export_dma_buf) {
    // Retry without export on drivers that advertise the extension but
    // refuse DMA-BUF-exportable allocations for this format.
    spdlog::warn(
        "VulkanBackingStore: DMA-BUF export alloc failed ({}); falling back",
        static_cast<int>(alloc_res));
    alloc_info.pNext = nullptr;
    alloc_res = d.vkAllocateMemory(device_, &alloc_info, nullptr, &memory_);
    export_dma_buf = false;
  }
  if (alloc_res != VK_SUCCESS) {
    spdlog::error("VulkanBackingStore: vkAllocateMemory failed ({})",
                  static_cast<int>(alloc_res));
    Destroy();
    return;
  }

  if (d.vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
    spdlog::error("VulkanBackingStore: vkBindImageMemory failed");
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
    spdlog::error("VulkanBackingStore: vkCreateImageView failed");
    Destroy();
    return;
  }

  if (export_dma_buf) {
    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory_;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (d.vkGetMemoryFdKHR) {
      VkResult r = d.vkGetMemoryFdKHR(device_, &fd_info, &dma_buf_fd_);
      if (r != VK_SUCCESS) {
        spdlog::warn("VulkanBackingStore: vkGetMemoryFdKHR failed ({})",
                     static_cast<int>(r));
        dma_buf_fd_ = -1;
      }
    }
  }

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
