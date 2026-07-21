/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "backend/hud/vulkan_hud.h"

#include <cstring>

// Shared dynamic dispatch (the backend owns/initialises the loader storage), so
// the HUD's own render-pass/framebuffer calls resolve through the same
// interposed loader as the rest of the compositor.
#define VULKAN_HPP_NO_EXCEPTIONS 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

namespace ihs::hud {

namespace {

const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

// imgui is built IMGUI_IMPL_VULKAN_NO_PROTOTYPES, so it resolves every Vulkan
// entry point through this loader — the interposed vkGetInstanceProcAddr, which
// serialises vkQueue* on the shared queue lock. One HUD, one imgui context.
struct LoaderCtx {
  PFN_vkGetInstanceProcAddr gipa{nullptr};
  VkInstance instance{VK_NULL_HANDLE};
};
LoaderCtx g_loader_ctx;

PFN_vkVoidFunction HudLoader(const char* name, void* user_data) {
  auto* c = static_cast<LoaderCtx*>(user_data);
  PFN_vkVoidFunction fn = c->gipa(c->instance, name);
  if (fn == nullptr && name != nullptr &&
      (std::strstr(name, "Swapchain") != nullptr ||
       std::strstr(name, "Surface") != nullptr)) {
    // imgui's LoadFunctions unconditionally requires the swapchain/surface
    // entry points for its ImGui_ImplVulkanH_* window helpers. We never use
    // those (the HUD records into the caller's render pass), and backends that
    // scan out without a swapchain — the DRM Vulkan backend presents via
    // dma-buf/KMS, so VK_KHR_swapchain/surface are not enabled — resolve them
    // to null. Hand back a non-null sentinel so LoadFunctions succeeds; it is
    // never dereferenced because the window helpers are never called.
    return reinterpret_cast<PFN_vkVoidFunction>(&HudLoader);
  }
  return fn;
}

}  // namespace

std::unique_ptr<VulkanHud> VulkanHud::Create(VkInstance instance,
                                             VkPhysicalDevice physical_device,
                                             VkDevice device,
                                             uint32_t queue_family,
                                             VkQueue queue,
                                             void* loader,
                                             VkFormat color_format,
                                             uint32_t image_count,
                                             std::string& err) {
  auto* gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader);
  if (gipa == nullptr) {
    err = "null loader";
    return nullptr;
  }

  std::unique_ptr<VulkanHud> hud(new VulkanHud());
  hud->device_ = device;

  // Render pass: one color attachment, LOAD (preserve the composited frame),
  // GENERAL in and out (the present path leaves the target image in GENERAL
  // after compositing and transitions it to PRESENT_SRC / scans it out after).
  VkAttachmentDescription att{};
  att.format = color_format;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
  att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp{};
  rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp.attachmentCount = 1;
  rp.pAttachments = &att;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  rp.dependencyCount = 1;
  rp.pDependencies = &dep;
  if (d().vkCreateRenderPass(device, &rp, nullptr, &hud->render_pass_) !=
      VK_SUCCESS) {
    err = "vkCreateRenderPass failed";
    return nullptr;
  }

  // Descriptor pool imgui allocates its font/texture sets from.
  const VkDescriptorPoolSize pool_size{
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
  VkDescriptorPoolCreateInfo dp{};
  dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dp.maxSets = 16;
  dp.poolSizeCount = 1;
  dp.pPoolSizes = &pool_size;
  if (d().vkCreateDescriptorPool(device, &dp, nullptr, &hud->desc_pool_) !=
      VK_SUCCESS) {
    err = "vkCreateDescriptorPool failed";
    d().vkDestroyRenderPass(device, hud->render_pass_, nullptr);
    return nullptr;
  }

  hud->InitContext();

  g_loader_ctx = LoaderCtx{gipa, instance};
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, HudLoader,
                                      &g_loader_ctx)) {
    // A required core entry point did not resolve. Fail gracefully rather than
    // letting ImGui_ImplVulkan_Init abort on its g_FunctionsLoaded assertion.
    err = "ImGui_ImplVulkan_LoadFunctions failed";
    ImGui::DestroyContext(static_cast<ImGuiContext*>(hud->imgui_ctx_));
    hud->imgui_ctx_ = nullptr;  // so ~VulkanHud skips ImGui_ImplVulkan_Shutdown
    d().vkDestroyDescriptorPool(device, hud->desc_pool_, nullptr);
    d().vkDestroyRenderPass(device, hud->render_pass_, nullptr);
    return nullptr;
  }

  ImGui_ImplVulkan_InitInfo init{};
  init.ApiVersion = VK_API_VERSION_1_1;
  init.Instance = instance;
  init.PhysicalDevice = physical_device;
  init.Device = device;
  init.QueueFamily = queue_family;
  init.Queue = queue;
  init.DescriptorPool = hud->desc_pool_;
  init.MinImageCount = image_count < 2 ? 2 : image_count;
  init.ImageCount = init.MinImageCount;
  init.PipelineInfoMain.RenderPass = hud->render_pass_;
  init.PipelineInfoMain.Subpass = 0;
  init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&init)) {
    err = "ImGui_ImplVulkan_Init failed";
    d().vkDestroyDescriptorPool(device, hud->desc_pool_, nullptr);
    d().vkDestroyRenderPass(device, hud->render_pass_, nullptr);
    return nullptr;
  }
  return hud;
}

VulkanHud::~VulkanHud() {
  // Shut the imgui render backend down while device_ is still valid (the caller
  // guarantees this — see WaylandVulkanBackend teardown). The base destructor
  // then frees the imgui context.
  if (imgui_ctx_ != nullptr) {
    MakeCurrent();
    ImGui_ImplVulkan_Shutdown();
  }
  for (auto& [view, fb] : framebuffers_) {
    d().vkDestroyFramebuffer(device_, fb, nullptr);
  }
  framebuffers_.clear();
  if (desc_pool_ != VK_NULL_HANDLE) {
    d().vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
  }
  if (render_pass_ != VK_NULL_HANDLE) {
    d().vkDestroyRenderPass(device_, render_pass_, nullptr);
  }
}

VkFramebuffer VulkanHud::FramebufferFor(VkImageView view,
                                        uint32_t width,
                                        uint32_t height) {
  if (const auto it = framebuffers_.find(view); it != framebuffers_.end()) {
    return it->second;
  }
  VkFramebufferCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fi.renderPass = render_pass_;
  fi.attachmentCount = 1;
  fi.pAttachments = &view;
  fi.width = width;
  fi.height = height;
  fi.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  d().vkCreateFramebuffer(device_, &fi, nullptr, &fb);
  framebuffers_[view] = fb;
  return fb;
}

void VulkanHud::DropFramebuffers() {
  for (auto& [view, fb] : framebuffers_) {
    d().vkDestroyFramebuffer(device_, fb, nullptr);
  }
  framebuffers_.clear();
}

void VulkanHud::ImplNewFrame() {
  ImGui_ImplVulkan_NewFrame();
}

void VulkanHud::ImplRenderDrawData(ImDrawData* draw_data) {
  VkRenderPassBeginInfo rb{};
  rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rb.renderPass = render_pass_;
  rb.framebuffer = FramebufferFor(cur_view_, cur_width_, cur_height_);
  rb.renderArea = {{0, 0}, {cur_width_, cur_height_}};
  d().vkCmdBeginRenderPass(cur_cmd_, &rb, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(draw_data, cur_cmd_);
  d().vkCmdEndRenderPass(cur_cmd_);
}

void VulkanHud::Render(VkCommandBuffer cmd,
                       VkImageView target_view,
                       uint32_t width,
                       uint32_t height,
                       float dt_seconds,
                       const HudStats& stats,
                       const std::vector<HudViewSample>& views) {
  cur_cmd_ = cmd;
  cur_view_ = target_view;
  cur_width_ = width;
  cur_height_ = height;
  RenderFrame(width, height, dt_seconds, stats, views);
}

}  // namespace ihs::hud
