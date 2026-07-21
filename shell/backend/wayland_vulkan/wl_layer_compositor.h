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

#ifndef SHELL_BACKEND_WAYLAND_VULKAN_WL_LAYER_COMPOSITOR_H_
#define SHELL_BACKEND_WAYLAND_VULKAN_WL_LAYER_COMPOSITOR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace wl_vulkan {

// Draws the Flutter layer stack into a dma-buf slot image with src-over alpha
// blending, so overlapping layers (a platform view under a transparent Flutter
// overlay) compose correctly — a plain vkCmdBlitImage overwrites, which turns
// the transparent overlay's holes black over the platform view.
//
// Each layer is a textured quad at its destination rect, blended premultiplied
// src-over into the slot (a COLOR_ATTACHMENT render target). One render pass
// per frame: BeginFrame -> DrawLayer per layer (in z-order) -> EndFrame.
class LayerCompositor {
 public:
  // Build the render pass / pipeline / sampler for @p color_format (the slot
  // format). Returns nullptr and sets @p err on failure.
  static std::unique_ptr<LayerCompositor> Create(VkDevice device,
                                                 VkFormat color_format,
                                                 std::string& err);
  ~LayerCompositor();
  LayerCompositor(const LayerCompositor&) = delete;
  LayerCompositor& operator=(const LayerCompositor&) = delete;

  // Begin compositing into @p slot_view (a view of the slot image, sized
  // @p width x @p height). @p frame is a monotonically increasing counter used
  // to recycle per-frame descriptors + source views once their GPU work has
  // retired. The render pass clears to opaque black, then layers blend on top.
  // Records into @p cmd (already begun); the slot image must be in
  // COLOR_ATTACHMENT_OPTIMAL on entry (the render pass leaves it in GENERAL).
  bool BeginFrame(VkCommandBuffer cmd,
                  VkImageView slot_view,
                  uint32_t width,
                  uint32_t height,
                  uint64_t frame);

  // Blend one layer: sample all of @p src_image (already in
  // SHADER_READ_ONLY_OPTIMAL, of format @p src_format — the view must match the
  // image's own format) into the destination rect (pixels, top-left origin —
  // matches the blit path, no Y flip).
  void DrawLayer(VkCommandBuffer cmd,
                 VkImage src_image,
                 VkFormat src_format,
                 int32_t dst_x,
                 int32_t dst_y,
                 int32_t dst_w,
                 int32_t dst_h);

  static void EndFrame(VkCommandBuffer cmd);

  // Destroy all cached framebuffers. Call when the target image views they were
  // built from are being destroyed (e.g. a swapchain resize) so a later
  // BeginFrame rebuilds them instead of reusing a framebuffer that references a
  // freed view. The caller must have made the GPU idle first.
  void ClearFramebuffers();

 private:
  explicit LayerCompositor(VkDevice device) : device_(device) {}
  VkImageView CreateSourceView(VkImage image, VkFormat format);
  VkFramebuffer GetOrCreateFramebuffer(VkImageView slot_view,
                                       uint32_t width,
                                       uint32_t height);

  VkDevice device_{VK_NULL_HANDLE};
  VkFormat color_format_{VK_FORMAT_UNDEFINED};
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkSampler sampler_{VK_NULL_HANDLE};

  // Current frame state.
  uint32_t fb_width_{0};
  uint32_t fb_height_{0};

  // Framebuffers keyed by slot image view (a handful of fixed slots).
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers_;

  // Descriptor pools + source views are recycled through a ring deep enough to
  // outlast in-flight frames, so a set/view is only destroyed after the frame
  // that used it has retired.
  static constexpr uint32_t kRing = 4;
  static constexpr uint32_t kMaxLayersPerFrame = 64;
  struct FrameResources {
    VkDescriptorPool pool{VK_NULL_HANDLE};
    std::vector<VkImageView> views;  // per-source views created this cycle
  };
  std::vector<FrameResources> ring_{kRing};
  uint32_t cur_ring_{0};
};

}  // namespace wl_vulkan

#endif  // SHELL_BACKEND_WAYLAND_VULKAN_WL_LAYER_COMPOSITOR_H_
