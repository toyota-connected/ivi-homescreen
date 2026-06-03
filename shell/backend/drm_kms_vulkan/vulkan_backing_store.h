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
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <shell/platform/embedder/embedder.h>

namespace drm_kms_vulkan {

// Per-memory-plane layout of an exported modifier image, used to import it as a
// KMS framebuffer (drmModeAddFB2WithModifiers) without a copy.
struct PlaneLayout {
  uint64_t offset = 0;
  uint64_t pitch = 0;
};

// Intersect the modifiers a Vulkan ICD can EXPORT for @p vk_format (as a
// transfer/color target) with @p plane_modifiers (a scanout plane's IN_FORMATS
// for the matching fourcc). Returns the common set ordered tiled-first with
// DRM_FORMAT_MOD_LINEAR last, so the driver prefers a compressed/tiled layout
// and only falls back to linear. Empty if there is no overlap.
std::vector<uint64_t> NegotiateModifiers(
    VkPhysicalDevice physical_device,
    VkFormat vk_format,
    const std::vector<uint64_t>& plane_modifiers);

// A device-local VkImage allocated with an explicit DRM format modifier and
// exported as a dma-buf, ready for zero-copy KMS scanout. The driver chooses a
// modifier from the negotiated set; the chosen modifier and per-plane layout
// are read back for the framebuffer import. Owns the image, memory, view, and
// the exported dma-buf fd; all are released in the destructor.
class VulkanBackingStore {
 public:
  // Create the exported modifier image. @p allowed_modifiers is the negotiated
  // set from NegotiateModifiers (must be non-empty). On failure returns nullptr
  // and sets @p err.
  static std::unique_ptr<VulkanBackingStore> Create(
      VkPhysicalDevice physical_device,
      VkDevice device,
      uint32_t width,
      uint32_t height,
      VkFormat vk_format,
      uint32_t drm_fourcc,
      const std::vector<uint64_t>& allowed_modifiers,
      std::string& err);

  ~VulkanBackingStore();

  VulkanBackingStore(const VulkanBackingStore&) = delete;
  VulkanBackingStore& operator=(const VulkanBackingStore&) = delete;

  [[nodiscard]] VkImage image() const { return image_; }
  [[nodiscard]] VkImageView view() const { return view_; }
  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  [[nodiscard]] VkFormat vk_format() const { return vk_format_; }
  [[nodiscard]] uint32_t drm_fourcc() const { return drm_fourcc_; }
  [[nodiscard]] uint64_t modifier() const { return modifier_; }
  [[nodiscard]] const std::vector<PlaneLayout>& planes() const {
    return planes_;
  }
  // Borrowed; the store owns it and closes it on destruction.
  [[nodiscard]] int dma_buf_fd() const { return dma_buf_fd_; }

  // Address-stable FlutterVulkanImage for the renderer config / backing store.
  [[nodiscard]] const FlutterVulkanImage* flutter_image() const {
    return &flutter_image_;
  }

 private:
  VulkanBackingStore(VkDevice device, uint32_t width, uint32_t height);

  VkDevice device_ = VK_NULL_HANDLE;  // not owned
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
  uint32_t drm_fourcc_ = 0;
  uint64_t modifier_ = 0;

  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  int dma_buf_fd_ = -1;
  std::vector<PlaneLayout> planes_;

  FlutterVulkanImage flutter_image_{};
};

}  // namespace drm_kms_vulkan
