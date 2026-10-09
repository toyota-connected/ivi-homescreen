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

#include <sys/types.h>

#include <array>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "ihs/platform_view.h"

// Imports a plugin-produced dma-buf (IhsFrame) into a VkImage on the
// compositor's own Vulkan device, so an ihs_pv platform view's output is a
// first-class Vulkan resource the compositor blits (and, later, hands to a
// wl_subsurface / KMS plane for a copy-free path). The imported image aliases
// the dma-buf's memory — no pixel copy; only the VkImage/VkDeviceMemory
// wrappers are created, once per ring buffer.
//
// All Vulkan entry points are resolved through the backend's interposed
// get_instance_proc_addr, so submissions are serialized on the shared queue the
// same way the engine's are. The caller owns the ImportedImage lifetimes; the
// importer keeps only the pool of freed imports described at Destroy.
class DmabufVulkanImporter {
 public:
  struct ImportedImage {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    // The format the image was created with, and whether it is a planar YUV
    // one. The compositor needs both to build the view and sampler: a YUV
    // image samples through a VkSamplerYcbcrConversion of this format, an RGB
    // image through a plain sampler. VK_FORMAT_UNDEFINED on an unset image.
    VkFormat format{VK_FORMAT_UNDEFINED};
    bool yuv{false};
    // For a YUV image, the color model and range the compositor's
    // VkSamplerYcbcrConversion must use, resolved from the frame's color_space/
    // color_range (IHS_COLOR_*_DEFAULT -> BT.601/709 by height, limited range).
    // Unused for RGB.
    VkSamplerYcbcrModelConversion ycbcr_model{
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY};
    VkSamplerYcbcrRange ycbcr_range{VK_SAMPLER_YCBCR_RANGE_ITU_NARROW};
    // False for an image the producer made on this device and keeps (Adopt):
    // no memory here, and Destroy hands the image back through @release
    // rather than destroying it.
    bool owns_image{true};
    void (*release)(void* user_data, void* image, uint32_t buffer_id){nullptr};
    void* release_user_data{nullptr};
    uint32_t buffer_id{0};
  };

  DmabufVulkanImporter() = default;

  // Resolve the device entry points from @get_instance_proc_addr. Returns false
  // when the device lacks the dma-buf import extensions (the plugin then falls
  // back to the software floor). Call once before Import.
  bool Init(VkInstance instance,
            VkPhysicalDevice physical_device,
            VkDevice device,
            void* get_instance_proc_addr);

  [[nodiscard]] bool ready() const { return device_ != VK_NULL_HANDLE; }

  // Import @frame's dma-buf into @out. On success the imported image has taken
  // ownership of frame.plane_fd[0] (freed with the memory in Destroy); on
  // failure the fd is left untouched for the caller to close.
  //
  // Packed RGB (one plane) and planar YUV whose planes share a single dma-buf
  // (NV12: two planes, one fd, offsets within the one allocation). A YUV image
  // must be sampled through a VkSamplerYcbcrConversion of ImportedImage::format
  // -- created in the compositor, since the conversion is baked into the
  // sampler and descriptor layout there, not here.
  //
  // A frame whose dma-buf is in the pool (see Destroy), with the same layout,
  // gets that import back, and its fd is closed.
  bool Import(const IhsFrame& frame, ImportedImage* out) const;

  // Describe the producer's @p vk image, created on this importer's device, in
  // @out, without importing anything: the image stays the producer's, so
  // Destroy neither frees nor pools it but calls its release instead. Call
  // Destroy only once no frame binds it, as for an import. Single-plane color
  // formats only; VK_FORMAT_UNDEFINED means VK_FORMAT_B8G8R8A8_UNORM.
  bool Adopt(const IhsVkImage& vk, ImportedImage* out) const;

  // Modifiers this device will actually import and sample for @p drm_fourcc,
  // asked with the same usage Import creates the image with. Best first: the
  // order the capability query offers them, so a producer that can honor a
  // preference lands on one the driver admits.
  //
  // Empty is a real answer -- the device advertises no modifier it will import
  // for that format -- and the caller keeps offering what it always did. A
  // modifier the driver merely tolerates still works here (#597); offering
  // nothing would take the view away entirely.
  [[nodiscard]] std::vector<uint64_t> ImportableModifiers(
      uint32_t drm_fourcc) const;

  // Free @image, or keep it for reuse. A producer that pools its buffers
  // submits the same dma-bufs to its next view, and every import costs more
  // than the wrappers: on some drivers each import and free leaves an fd open
  // for good. So the last kPooledImports freed imports are kept, keyed by the
  // dma-buf they hold, and the oldest is freed past that. A pooled import
  // holds its dma-buf, so the key cannot come back as another buffer.
  //
  // Call only once no frame binds @image: the caller's deferred free has run.
  void Destroy(ImportedImage* image) const;

  // Free the pool, and stop pooling until the next Init. Call before the
  // device goes; frees after this are immediate.
  void DrainPool() const;

  static constexpr size_t kPooledImports = 16;

#if defined(UNIT_TEST)
  [[nodiscard]] size_t PooledForTest() const {
    const std::lock_guard<std::mutex> lock(pool_mu_);
    return pool_.size();
  }
#endif

  // A driver can require more of an imported dma-buf than the image it backs.
  // v3dv charges the import for its TFU read-ahead and rounds to a page, while
  // v3d's own GBM allocator adds no such slack -- so an image whose size
  // leaves fewer than @padding spare bytes in its last page cannot be imported
  // at req.size, the only allocationSize a dedicated allocation may use
  // (#723, mesa/mesa#16524). These two state that arithmetic; Import applies
  // it. @page is the driver's page constant, not the host's.

  // Bytes the dma-buf must hold for the driver to import an image of
  // @image_bytes, where it adds @padding to the allocation before checking.
  // Mesa 25.0.7's v3dv does, in v3dv_AllocateMemory; a driver that adds
  // nothing needs only the allocation itself. The refusal log reports this as
  // an upper bound, because a main-style v3dv (see below) demands a page less.
  [[nodiscard]] static constexpr VkDeviceSize ImportFootprint(
      VkDeviceSize image_bytes,
      VkDeviceSize padding,
      VkDeviceSize page = 4096) {
    if (padding == 0) {
      return image_bytes;
    }
    return ((image_bytes + padding + page - 1) / page) * page;
  }

  // The allocationSize to retry a refused import with, or 0 when no retry can
  // succeed. Asking @padding short brings the footprint down to the image's
  // own size rounded to a page, which a dma-buf holding @held bytes may
  // already cover; see Import for why that is out of spec.
  //
  // The condition is also what keeps this from papering over a producer that
  // under-allocated: a dma-buf is page-rounded, so requiring it to cover
  // align(@image_bytes, @page) is requiring it to hold the image. Offering a
  // retry merely because the shorter ask would be backed would hand the
  // driver an image whose last bytes are outside the buffer.
  //
  // Where Mesa main will need more than this: it reports the read-ahead inside
  // VkMemoryRequirements::size instead of adding it at allocation time, so
  // @image_bytes arrives 64 bytes past the modifier's layout and this declines
  // the retry -- correctly, on what it can see, since nothing here can tell
  // scratch padding from image. Deciding it needs the image's real extent from
  // vkGetImageSubresourceLayout (offset + size over the memory planes) in
  // place of req.size. Not done while the hardware we run is on 25.0.7, where
  // the two are equal.
  [[nodiscard]] static constexpr VkDeviceSize ShortImportSize(
      VkDeviceSize image_bytes,
      VkDeviceSize held,
      VkDeviceSize padding,
      VkDeviceSize page = 4096) {
    if (padding == 0 || image_bytes <= padding) {
      return 0;
    }
    const VkDeviceSize ask = image_bytes - padding;
    return held >= ImportFootprint(ask, padding, page) ? ask : 0;
  }

 private:
  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};

  PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties_{nullptr};
  PFN_vkCreateImage create_image_{nullptr};
  PFN_vkDestroyImage destroy_image_{nullptr};
  PFN_vkGetImageMemoryRequirements get_image_memory_requirements_{nullptr};
  PFN_vkAllocateMemory allocate_memory_{nullptr};
  PFN_vkFreeMemory free_memory_{nullptr};
  PFN_vkBindImageMemory bind_image_memory_{nullptr};
  PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_{nullptr};
  PFN_vkGetImageSubresourceLayout get_image_subresource_layout_{nullptr};
  PFN_vkGetPhysicalDeviceFormatProperties2 get_format_properties2_{nullptr};
  PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format_properties2_{
      nullptr};

  // Bytes the driver adds to an import's allocationSize before checking it
  // against the dma-buf; see Import. Nonzero on v3dv only.
  VkDeviceSize import_padding_{0};
  // Import a LINEAR frame into a LINEAR-tiled image rather than a
  // DRM_FORMAT_MODIFIER one; see Init. VeriSilicon only.
  bool linear_tiling_for_linear_{false};

  // What an import was made from: the dma-buf and the layout read from it.
  struct PoolKey {
    dev_t dev{0};
    ino_t ino{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t fourcc{0};
    uint64_t modifier{0};
    uint32_t plane_count{0};
    std::array<uint32_t, 2> offset{};
    std::array<uint32_t, 2> stride{};
    bool operator==(const PoolKey& o) const {
      return dev == o.dev && ino == o.ino && width == o.width &&
             height == o.height && fourcc == o.fourcc &&
             modifier == o.modifier && plane_count == o.plane_count &&
             offset == o.offset && stride == o.stride;
    }
  };
  static bool KeyOf(const IhsFrame& frame, PoolKey* out);

  bool ImportNew(const IhsFrame& frame, ImportedImage* out) const;
  void Free(ImportedImage* image) const;

  mutable std::mutex pool_mu_;
  // Every live import made from a dma-buf that could be keyed, by image.
  mutable std::unordered_map<VkImage, PoolKey> keys_;
  // Freed imports, newest first.
  mutable std::list<std::pair<PoolKey, ImportedImage>> pool_;
  mutable bool draining_{false};
};
