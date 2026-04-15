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

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include <shell/platform/embedder/embedder.h>

/**
 * @brief VkImage-backed Vulkan backing store.
 *
 * Allocates a device-local 2D color image plus backing @c VkDeviceMemory and
 * a @c VkImageView. Tracks the last-known layout so the compositor can record
 * correct barriers when the engine hands the image back (it renders with the
 * image in @c VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL).
 *
 * Lifetime: constructed on the rasterizer thread when @c Acquire is called;
 * destroyed when the pool is flushed or the store is released and the cap is
 * hit. The owning backend must outlive the store.
 *
 * When @p export_dma_buf is true at construction time, memory is allocated
 * with @c VK_KHR_external_memory_fd. @c dma_buf_fd() returns the fd (owned
 * by this object; @c Destroy closes it). Silently falls back to a non-export
 * allocation if the extension is not present or allocation with export flags
 * fails — call @c has_dma_buf() to detect.
 */
class VulkanBackingStore {
 public:
  static constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

  VulkanBackingStore(int32_t width,
                     int32_t height,
                     VkDevice device,
                     VkPhysicalDevice physical_device,
                     bool export_dma_buf);
  ~VulkanBackingStore();

  VulkanBackingStore(const VulkanBackingStore&) = delete;
  VulkanBackingStore& operator=(const VulkanBackingStore&) = delete;

  [[nodiscard]] int32_t Width() const { return width_; }
  [[nodiscard]] int32_t Height() const { return height_; }
  [[nodiscard]] VkImage Image() const { return image_; }
  [[nodiscard]] VkImageView View() const { return view_; }
  [[nodiscard]] VkImageLayout Layout() const { return layout_; }
  void SetLayout(VkImageLayout layout) { layout_ = layout; }
  [[nodiscard]] bool IsValid() const { return image_ != VK_NULL_HANDLE; }

  [[nodiscard]] bool has_dma_buf() const { return dma_buf_fd_ >= 0; }
  [[nodiscard]] int dma_buf_fd() const { return dma_buf_fd_; }

  /**
   * @brief FlutterVulkanImage wrapping this store's VkImage.
   *
   * Populated on construction; address-stable for the life of the object.
   */
  [[nodiscard]] const FlutterVulkanImage* engine_image() const {
    return &engine_image_;
  }

 private:
  void Destroy();

  int32_t width_{0};
  int32_t height_{0};
  VkDevice device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  int dma_buf_fd_{-1};

  FlutterVulkanImage engine_image_{};
};
