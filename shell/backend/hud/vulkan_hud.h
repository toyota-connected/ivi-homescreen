/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "backend/hud/hud_base.h"

namespace ihs::hud {

// Vulkan render backend for the debug HUD (imgui_impl_vulkan, loader-based).
// The shell hands it the interposed vkGetInstanceProcAddr, so imgui's Vulkan
// calls take the shared queue lock like the rest of the compositor. Recording
// runs on the raster/present thread inside the caller's command buffer; input
// is fed from the platform thread via the IHud methods.
class VulkanHud : public HudBase {
 public:
  // @loader is the interposed PFN_vkGetInstanceProcAddr. @color_format is the
  // target image format; the HUD render pass loads (preserves) it. Returns
  // nullptr with @err set on failure.
  static std::unique_ptr<VulkanHud> Create(VkInstance instance,
                                           VkPhysicalDevice physical_device,
                                           VkDevice device,
                                           uint32_t queue_family,
                                           VkQueue queue,
                                           void* loader,
                                           VkFormat color_format,
                                           uint32_t image_count,
                                           std::string& err);
  ~VulkanHud() override;

  VulkanHud(const VulkanHud&) = delete;
  VulkanHud& operator=(const VulkanHud&) = delete;

  // Record the HUD over @target_view (@width x @height, image in GENERAL layout
  // on entry and left in GENERAL) into @cmd.
  void Render(VkCommandBuffer cmd,
              VkImageView target_view,
              uint32_t width,
              uint32_t height,
              float dt_seconds,
              const HudStats& stats,
              const std::vector<HudViewSample>& views);

  // Drop cached framebuffers whose views were freed (swapchain resize).
  void DropFramebuffers();

 protected:
  void ImplNewFrame() override;
  void ImplRenderDrawData(ImDrawData* draw_data) override;

 private:
  VulkanHud() = default;
  VkFramebuffer FramebufferFor(VkImageView view,
                               uint32_t width,
                               uint32_t height);

  VkDevice device_{VK_NULL_HANDLE};
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkDescriptorPool desc_pool_{VK_NULL_HANDLE};
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers_;

  // Set by Render() for the current frame; consumed by ImplRenderDrawData.
  VkCommandBuffer cur_cmd_{VK_NULL_HANDLE};
  VkImageView cur_view_{VK_NULL_HANDLE};
  uint32_t cur_width_{0};
  uint32_t cur_height_{0};
};

}  // namespace ihs::hud
