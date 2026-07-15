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

#ifndef SHELL_BACKEND_WAYLAND_VULKAN_WL_DMABUF_PRESENT_H_
#define SHELL_BACKEND_WAYLAND_VULKAN_WL_DMABUF_PRESENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct wl_proxy;

namespace wl_vulkan {

// Per-memory-plane layout of an exported dma-buf image.
struct PlaneLayout {
  uint64_t offset = 0;
  uint64_t pitch = 0;
};

// Intersect the modifiers this Vulkan device can EXPORT for @p vk_format as a
// transfer/color target with the compositor's advertised @p
// compositor_modifiers (from the dma-buf feedback). Returns the common set,
// tiled-first / LINEAR-last (the driver prefers a compressed/tiled layout).
// Empty when there is no overlap — the caller then stays on the WSI swapchain
// path.
std::vector<uint64_t> NegotiateModifiers(
    VkPhysicalDevice physical_device,
    VkFormat vk_format,
    const std::vector<uint64_t>& compositor_modifiers);

// A modifier-tiled, dma-buf-exported VkImage the compositor can import (and
// promote to a hardware plane) as a wl_buffer. The engine's composited frame is
// blitted into it before present. Owns its Vulkan objects + the dma-buf fd.
class DmabufImage {
 public:
  // Allocate an exported image constrained to @p allowed_modifiers (the driver
  // picks one, read back into modifier()). @p usage must include the flags the
  // caller needs (e.g. TRANSFER_DST for a blit target). Returns nullptr and
  // sets
  // @p err on failure.
  static std::unique_ptr<DmabufImage> Create(
      VkPhysicalDevice physical_device,
      VkDevice device,
      uint32_t width,
      uint32_t height,
      VkFormat vk_format,
      uint32_t drm_fourcc,
      VkImageUsageFlags usage,
      const std::vector<uint64_t>& allowed_modifiers,
      std::string& err);

  DmabufImage(const DmabufImage&) = delete;
  DmabufImage& operator=(const DmabufImage&) = delete;
  ~DmabufImage();

  [[nodiscard]] VkImage image() const { return image_; }
  [[nodiscard]] VkImageView view() const { return view_; }
  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  [[nodiscard]] uint32_t drm_fourcc() const { return drm_fourcc_; }
  [[nodiscard]] uint64_t modifier() const { return modifier_; }
  [[nodiscard]] const std::vector<PlaneLayout>& planes() const {
    return planes_;
  }
  // Owned; closed in the destructor. Callers that hand it to
  // zwp_linux_buffer_params (which dups it) must NOT close it.
  [[nodiscard]] int dma_buf_fd() const { return dma_buf_fd_; }

 private:
  DmabufImage(VkDevice device, uint32_t width, uint32_t height)
      : device_(device), width_(width), height_(height) {}

  VkDevice device_{VK_NULL_HANDLE};
  uint32_t width_;
  uint32_t height_;
  uint32_t drm_fourcc_{0};
  uint64_t modifier_{0};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  int dma_buf_fd_{-1};
  std::vector<PlaneLayout> planes_;
};

// A wl_buffer backed by a DmabufImage's dma-buf, for wl_surface.attach. Tracks
// wl_buffer.release so the owning slot is not re-rendered while the compositor
// is still scanning out of it. The wayland-client wrapper is hidden behind a
// PIMPL so this header stays free of the generated protocol types.
class WlDmabufBuffer {
 public:
  // Build the buffer from @p img: one zwp_linux_buffer_params add() per memory
  // plane (all sharing the fd, which libwayland dups), then create_immed.
  // @p params_proxy comes from wl::DmabufFeedback::CreateParams(); this
  // consumes it. Returns nullptr on failure. The DmabufImage must outlive the
  // buffer.
  static std::unique_ptr<WlDmabufBuffer> Create(wl_proxy* params_proxy,
                                                const DmabufImage& img);
  ~WlDmabufBuffer();
  WlDmabufBuffer(const WlDmabufBuffer&) = delete;
  WlDmabufBuffer& operator=(const WlDmabufBuffer&) = delete;

  // The wl_buffer proxy to hand to wl_surface.attach.
  [[nodiscard]] wl_proxy* proxy() const;
  // False while the compositor holds the buffer (attached, not yet released).
  [[nodiscard]] bool released() const;
  // Call at present, after attach, to mark the buffer in the compositor's
  // hands.
  void mark_in_use();

 private:
  WlDmabufBuffer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wl_vulkan

#endif  // SHELL_BACKEND_WAYLAND_VULKAN_WL_DMABUF_PRESENT_H_
