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

#include "backend/wayland_vulkan/wl_layer_compositor.h"

#include <array>

// Dynamic dispatch, shared with the backend (which owns the loader storage).
#define VULKAN_HPP_NO_EXCEPTIONS 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

namespace wl_vulkan {

namespace {

const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

// Precompiled SPIR-V (glslc -O -mfmt=c shaders/composite.{vert,frag}).
// Regenerate with the same command if the .glsl sources change.
constexpr uint32_t kVertSpv[] =
#include "shaders/composite.vert.inc"
    ;
constexpr uint32_t kFragSpv[] =
#include "shaders/composite.frag.inc"
    ;

VkShaderModule MakeModule(VkDevice dev, const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  d().vkCreateShaderModule(dev, &ci, nullptr, &m);
  return m;
}

}  // namespace

std::unique_ptr<LayerCompositor> LayerCompositor::Create(VkDevice device,
                                                         VkFormat color_format,
                                                         std::string& err) {
  std::unique_ptr<LayerCompositor> c(new LayerCompositor(device));
  c->color_format_ = color_format;

  // Render pass: one color attachment (the slot), cleared then blended into,
  // left in GENERAL for the dma-buf present. initialLayout UNDEFINED because
  // the clear discards any prior content.
  VkAttachmentDescription att{};
  att.format = color_format;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  // External -> subpass: the color writes wait for prior transfer/host work
  // that produced the sources; subpass -> external hands the result to the
  // present read.
  std::array<VkSubpassDependency, 2> deps{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  VkRenderPassCreateInfo rp{};
  rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp.attachmentCount = 1;
  rp.pAttachments = &att;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  rp.dependencyCount = static_cast<uint32_t>(deps.size());
  rp.pDependencies = deps.data();
  if (d().vkCreateRenderPass(device, &rp, nullptr, &c->render_pass_) !=
      VK_SUCCESS) {
    err = "vkCreateRenderPass failed";
    return nullptr;
  }

  // set 0, binding 0: combined image sampler (the layer's source).
  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo sl{};
  sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  sl.bindingCount = 1;
  sl.pBindings = &b;
  if (d().vkCreateDescriptorSetLayout(device, &sl, nullptr, &c->set_layout_) !=
      VK_SUCCESS) {
    err = "vkCreateDescriptorSetLayout failed";
    return nullptr;
  }

  // Push constant: the destination rect in NDC (vec4).
  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pc.offset = 0;
  pc.size = sizeof(float) * 4;
  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &c->set_layout_;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pc;
  if (d().vkCreatePipelineLayout(device, &pl, nullptr, &c->pipeline_layout_) !=
      VK_SUCCESS) {
    err = "vkCreatePipelineLayout failed";
    return nullptr;
  }

  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (d().vkCreateSampler(device, &si, nullptr, &c->sampler_) != VK_SUCCESS) {
    err = "vkCreateSampler failed";
    return nullptr;
  }

  // Graphics pipeline: full-screen-ish quad, premultiplied src-over blend,
  // dynamic viewport/scissor, no vertex input (positions from gl_VertexIndex).
  VkShaderModule vs = MakeModule(device, kVertSpv, sizeof(kVertSpv));
  VkShaderModule fs = MakeModule(device, kFragSpv, sizeof(kFragSpv));
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;  // premultiplied
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  std::array<VkDynamicState, 2> dyn = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  ds.dynamicStateCount = static_cast<uint32_t>(dyn.size());
  ds.pDynamicStates = dyn.data();

  VkGraphicsPipelineCreateInfo gp{};
  gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gp.stageCount = static_cast<uint32_t>(stages.size());
  gp.pStages = stages.data();
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &ds;
  gp.layout = c->pipeline_layout_;
  gp.renderPass = c->render_pass_;
  gp.subpass = 0;
  const VkResult pr = d().vkCreateGraphicsPipelines(
      device, VK_NULL_HANDLE, 1, &gp, nullptr, &c->pipeline_);
  d().vkDestroyShaderModule(device, vs, nullptr);
  d().vkDestroyShaderModule(device, fs, nullptr);
  if (pr != VK_SUCCESS) {
    err = "vkCreateGraphicsPipelines failed";
    return nullptr;
  }

  // One descriptor pool per ring slot, recycled in BeginFrame.
  for (auto& fr : c->ring_) {
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = kMaxLayersPerFrame;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kMaxLayersPerFrame;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (d().vkCreateDescriptorPool(device, &pi, nullptr, &fr.pool) !=
        VK_SUCCESS) {
      err = "vkCreateDescriptorPool failed";
      return nullptr;
    }
  }
  return c;
}

LayerCompositor::~LayerCompositor() {
  for (auto& [view, fb] : framebuffers_) {
    d().vkDestroyFramebuffer(device_, fb, nullptr);
  }
  for (auto& fr : ring_) {
    for (VkImageView v : fr.views) {
      d().vkDestroyImageView(device_, v, nullptr);
    }
    if (fr.pool != VK_NULL_HANDLE) {
      d().vkDestroyDescriptorPool(device_, fr.pool, nullptr);
    }
  }
  if (pipeline_ != VK_NULL_HANDLE) {
    d().vkDestroyPipeline(device_, pipeline_, nullptr);
  }
  if (pipeline_layout_ != VK_NULL_HANDLE) {
    d().vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  }
  if (set_layout_ != VK_NULL_HANDLE) {
    d().vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  }
  if (sampler_ != VK_NULL_HANDLE) {
    d().vkDestroySampler(device_, sampler_, nullptr);
  }
  if (render_pass_ != VK_NULL_HANDLE) {
    d().vkDestroyRenderPass(device_, render_pass_, nullptr);
  }
}

VkImageView LayerCompositor::CreateSourceView(VkImage image, VkFormat format) {
  // The view format must match the image's own format (backing stores are
  // R8G8B8A8, platform-view images B8G8R8A8); the sampler reads logical RGBA
  // either way and the attachment format handles the byte order, so no swizzle.
  VkImageViewCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ci.image = image;
  ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ci.format = format;
  ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v = VK_NULL_HANDLE;
  d().vkCreateImageView(device_, &ci, nullptr, &v);
  ring_[cur_ring_].views.push_back(v);
  return v;
}

VkFramebuffer LayerCompositor::GetOrCreateFramebuffer(VkImageView slot_view,
                                                      uint32_t width,
                                                      uint32_t height) {
  const auto it = framebuffers_.find(slot_view);
  if (it != framebuffers_.end()) {
    return it->second;
  }
  VkFramebufferCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fi.renderPass = render_pass_;
  fi.attachmentCount = 1;
  fi.pAttachments = &slot_view;
  fi.width = width;
  fi.height = height;
  fi.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  d().vkCreateFramebuffer(device_, &fi, nullptr, &fb);
  framebuffers_[slot_view] = fb;
  return fb;
}

bool LayerCompositor::BeginFrame(VkCommandBuffer cmd,
                                 VkImageView slot_view,
                                 uint32_t width,
                                 uint32_t height,
                                 uint64_t frame) {
  fb_width_ = width;
  fb_height_ = height;
  cur_ring_ = static_cast<uint32_t>(frame % kRing);

  // Recycle this ring slot: its resources are from `kRing` frames ago, whose
  // GPU work has retired. Free the source views and reset the descriptor pool.
  auto& fr = ring_[cur_ring_];
  for (VkImageView v : fr.views) {
    d().vkDestroyImageView(device_, v, nullptr);
  }
  fr.views.clear();
  d().vkResetDescriptorPool(device_, fr.pool, 0);

  VkFramebuffer fb = GetOrCreateFramebuffer(slot_view, width, height);
  if (fb == VK_NULL_HANDLE) {
    return false;
  }

  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};  // opaque black
  VkRenderPassBeginInfo rb{};
  rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rb.renderPass = render_pass_;
  rb.framebuffer = fb;
  rb.renderArea = {{0, 0}, {width, height}};
  rb.clearValueCount = 1;
  rb.pClearValues = &clear;
  d().vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);

  d().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  VkViewport viewport{
      0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
      0.0f, 1.0f};
  d().vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {width, height}};
  d().vkCmdSetScissor(cmd, 0, 1, &scissor);
  return true;
}

void LayerCompositor::DrawLayer(VkCommandBuffer cmd,
                                VkImage src_image,
                                VkFormat src_format,
                                int32_t dst_x,
                                int32_t dst_y,
                                int32_t dst_w,
                                int32_t dst_h) {
  if (fb_width_ == 0 || fb_height_ == 0) {
    return;
  }
  VkImageView view = CreateSourceView(src_image, src_format);

  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = ring_[cur_ring_].pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (d().vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
    return;  // pool exhausted (> kMaxLayersPerFrame) — skip this layer
  }
  VkDescriptorImageInfo ii{};
  ii.sampler = sampler_;
  ii.imageView = view;
  ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = set;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &ii;
  d().vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
  d().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_layout_, 0, 1, &set, 0, nullptr);

  // Destination rect -> NDC (top-left origin, y down; matches the blit path).
  const auto fw = static_cast<float>(fb_width_);
  const auto fh = static_cast<float>(fb_height_);
  const std::array<float, 4> rect = {
      2.0f * static_cast<float>(dst_x) / fw - 1.0f,
      2.0f * static_cast<float>(dst_y) / fh - 1.0f,
      2.0f * static_cast<float>(dst_x + dst_w) / fw - 1.0f,
      2.0f * static_cast<float>(dst_y + dst_h) / fh - 1.0f};
  d().vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                         sizeof(rect), rect.data());
  d().vkCmdDraw(cmd, 4, 1, 0, 0);
}

void LayerCompositor::EndFrame(VkCommandBuffer cmd) {
  d().vkCmdEndRenderPass(cmd);
}

void LayerCompositor::ClearFramebuffers() {
  for (auto& [view, fb] : framebuffers_) {
    d().vkDestroyFramebuffer(device_, fb, nullptr);
  }
  framebuffers_.clear();
}

}  // namespace wl_vulkan
