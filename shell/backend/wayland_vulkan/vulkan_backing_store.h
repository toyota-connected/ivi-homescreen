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
#include <optional>
#include <vector>

#include <vulkan/vulkan.h>

#include <shell/platform/embedder/embedder.h>

/** @brief One memory plane of an exported image, as the driver laid it out. */
struct VulkanStoreDmaBufPlane {
  uint64_t offset;
  uint64_t stride;
};

/**
 * @brief Whether and how a store can export its memory as a dma-buf.
 *
 * Decided by query rather than assumed. A dma-buf handle type has to be
 * reported compatible with the exact format, tiling and usage the image is
 * created with; most drivers report it only for
 * @c VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, and asking for the export
 * anyway is out of spec even where the driver lets the calls through
 * (VUID-VkImageCreateInfo-pNext-00990,
 * VUID-VkExportMemoryAllocateInfo-handleTypes-09860). Build one with
 * @c VulkanBackingStore::PlanExport.
 */
struct VulkanStoreExportPlan {
  bool enabled{false};
  VkImageTiling tiling{VK_IMAGE_TILING_OPTIMAL};
  /// Candidates for VkImageDrmFormatModifierListCreateInfoEXT, driver's choice
  /// among them. Empty unless @c tiling is DRM_FORMAT_MODIFIER.
  std::vector<uint64_t> modifiers{};
};

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
 * When the export plan passed at construction time is enabled, memory is
 * allocated with @c VK_KHR_external_memory_fd. @c dma_buf_fd() returns the fd
 * (owned by this object; @c Destroy closes it). Silently falls back to a
 * non-export allocation if the allocation with export flags fails — call
 * @c has_dma_buf() to detect.
 */
class VulkanBackingStore {
 public:
  static constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

  /**
   * @brief Usage set every store is created with.
   *
   * One image serves both present paths, so this is the union of what they
   * need: COLOR_ATTACHMENT because the engine renders the layer into it,
   * TRANSFER_SRC because the copy path blits it into the swapchain image
   * (BlitStoreToSwapchain), and SAMPLED because the layer-compositor path
   * draws it as a texture instead. TRANSFER_DST is carried as well, though
   * nothing in this backend currently copies into a store.
   *
   * Public because @c PlanExport has to query with the same set the image is
   * then created with, or the answer is about a different image.
   */
  static constexpr VkImageUsageFlags kUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  /**
   * @brief Decide how (or whether) stores on this device can export a dma-buf.
   *
   * Cheap but not free (a handful of physical-device queries); call once per
   * device, not once per store. @p have_drm_format_modifier_ext says whether
   * VK_EXT_image_drm_format_modifier is enabled on the device — without it the
   * modifier path cannot be queried, let alone taken.
   *
   * Returns a disabled plan when nothing is exportable, which is not an error:
   * stores then render exactly as before and @c has_dma_buf() stays false.
   */
  static VulkanStoreExportPlan PlanExport(VkPhysicalDevice physical_device,
                                          bool have_drm_format_modifier_ext);

  VulkanBackingStore(int32_t width,
                     int32_t height,
                     VkDevice device,
                     VkPhysicalDevice physical_device,
                     const VulkanStoreExportPlan& export_plan);
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
   * @brief DRM format modifier the driver chose for the exported image.
   *
   * Empty on the opaque-tiling export path, where there is no modifier to
   * name and an importer has to already know the layout, and when there is no
   * export at all.
   */
  [[nodiscard]] std::optional<uint64_t> dma_buf_modifier() const {
    return dma_buf_modifier_;
  }

  /**
   * @brief Per-plane offset and stride of the exported image.
   *
   * Populated on the modifier path only, one entry per memory plane of the
   * chosen modifier. An importer needs these along with @c dma_buf_fd() and
   * @c dma_buf_modifier() to describe the buffer.
   */
  [[nodiscard]] const std::vector<VulkanStoreDmaBufPlane>& dma_buf_planes()
      const {
    return dma_buf_planes_;
  }

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
  std::optional<uint64_t> dma_buf_modifier_{};
  std::vector<VulkanStoreDmaBufPlane> dma_buf_planes_{};

  FlutterVulkanImage engine_image_{};
};
