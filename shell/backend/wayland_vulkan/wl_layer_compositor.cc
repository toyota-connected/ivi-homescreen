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

#include "logging/logging.h"

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

// Whether the compositor samples @p format through a VkSamplerYcbcrConversion.
// Matches the planar formats DmabufVulkanImporter maps YUV fourccs to.
bool IsYuvFormat(VkFormat format) {
  return format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
}

// Builds the compositing pipeline for @p layout. The RGB and YUV variants share
// identical fixed-function state (a full quad, premultiplied src-over blend,
// dynamic viewport/scissor, positions from gl_VertexIndex, no vertex input) and
// the same shaders; they differ only in the layout — the YUV one references the
// immutable-ycbcr-sampler set layout, which bakes the conversion into sampling.
// Returns VK_NULL_HANDLE on failure.
VkPipeline MakePipeline(VkDevice device,
                        VkRenderPass render_pass,
                        VkPipelineLayout layout) {
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
  gp.layout = layout;
  gp.renderPass = render_pass;
  gp.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult pr = d().vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                    &gp, nullptr, &pipeline);
  d().vkDestroyShaderModule(device, vs, nullptr);
  d().vkDestroyShaderModule(device, fs, nullptr);
  return pr == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
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

  // Graphics pipeline (see MakePipeline): full quad, premultiplied src-over
  // blend, dynamic viewport/scissor, no vertex input. The YUV variants are
  // built lazily against their own layouts in GetOrCreateYuvProgram.
  c->pipeline_ = MakePipeline(device, c->render_pass_, c->pipeline_layout_);
  if (c->pipeline_ == VK_NULL_HANDLE) {
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
  for (const YuvProgram& p : yuv_programs_) {
    // A cached negative result (ok == false) holds only null handles; guard
    // each destroy like the rest of this destructor rather than relying on
    // Vulkan accepting null.
    if (p.pipeline != VK_NULL_HANDLE) {
      d().vkDestroyPipeline(device_, p.pipeline, nullptr);
    }
    if (p.pipeline_layout != VK_NULL_HANDLE) {
      d().vkDestroyPipelineLayout(device_, p.pipeline_layout, nullptr);
    }
    if (p.set_layout != VK_NULL_HANDLE) {
      d().vkDestroyDescriptorSetLayout(device_, p.set_layout, nullptr);
    }
    if (p.sampler != VK_NULL_HANDLE) {
      d().vkDestroySampler(device_, p.sampler, nullptr);
    }
    if (p.conversion != VK_NULL_HANDLE) {
      d().vkDestroySamplerYcbcrConversion(device_, p.conversion, nullptr);
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

VkImageView LayerCompositor::CreateSourceView(
    VkImage image,
    VkFormat format,
    VkSamplerYcbcrConversion conversion) {
  // The view format must match the image's own format (backing stores are
  // R8G8B8A8, platform-view images B8G8R8A8, YUV images their planar format);
  // the sampler reads logical RGBA either way and the attachment format handles
  // the byte order, so no swizzle. A YUV view carries the same conversion as
  // the immutable sampler it will be paired with — required for a view of a
  // multi-planar format used with ycbcr sampling.
  VkSamplerYcbcrConversionInfo conv_info{};
  conv_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
  conv_info.conversion = conversion;
  VkImageViewCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ci.pNext = conversion != VK_NULL_HANDLE ? &conv_info : nullptr;
  ci.image = image;
  ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ci.format = format;
  ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v = VK_NULL_HANDLE;
  if (d().vkCreateImageView(device_, &ci, nullptr, &v) != VK_SUCCESS) {
    return VK_NULL_HANDLE;  // caller skips the layer; nothing to record/destroy
  }
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

  // Viewport/scissor are dynamic and persist across the pipeline binds each
  // DrawLayer issues (RGB vs YUV layers use different pipelines); set them once
  // here. The pipeline itself is bound per layer, not here.
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
                                VkSamplerYcbcrModelConversion ycbcr_model,
                                VkSamplerYcbcrRange ycbcr_range,
                                int32_t dst_x,
                                int32_t dst_y,
                                int32_t dst_w,
                                int32_t dst_h) {
  if (fb_width_ == 0 || fb_height_ == 0) {
    return;
  }

  // RGB layers use the shared pipeline/sampler; a YUV layer resolves (or
  // builds) the program whose immutable ycbcr sampler bakes the conversion, and
  // its view carries the same conversion. With an immutable sampler the
  // descriptor write leaves the sampler null (the layout supplies it).
  VkPipeline pipeline = pipeline_;
  VkPipelineLayout pipeline_layout = pipeline_layout_;
  VkDescriptorSetLayout set_layout = set_layout_;
  VkSampler sampler = sampler_;
  VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
  if (IsYuvFormat(src_format)) {
    const YuvProgram* prog =
        GetOrCreateYuvProgram(src_format, ycbcr_model, ycbcr_range);
    if (prog == nullptr) {
      return;  // conversion/pipeline unavailable — skip rather than draw wrong
    }
    pipeline = prog->pipeline;
    pipeline_layout = prog->pipeline_layout;
    set_layout = prog->set_layout;
    sampler = VK_NULL_HANDLE;  // immutable in the layout
    conversion = prog->conversion;
  }

  VkImageView view = CreateSourceView(src_image, src_format, conversion);
  if (view == VK_NULL_HANDLE) {
    return;  // view creation failed — skip this layer rather than bind null
  }

  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = ring_[cur_ring_].pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (d().vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
    return;  // pool exhausted (> kMaxLayersPerFrame) — skip this layer
  }
  VkDescriptorImageInfo ii{};
  ii.sampler = sampler;  // ignored for the YUV binding's immutable sampler
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
  d().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  d().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_layout, 0, 1, &set, 0, nullptr);

  // Destination rect -> NDC (top-left origin, y down; matches the blit path).
  const auto fw = static_cast<float>(fb_width_);
  const auto fh = static_cast<float>(fb_height_);
  const std::array<float, 4> rect = {
      2.0f * static_cast<float>(dst_x) / fw - 1.0f,
      2.0f * static_cast<float>(dst_y) / fh - 1.0f,
      2.0f * static_cast<float>(dst_x + dst_w) / fw - 1.0f,
      2.0f * static_cast<float>(dst_y + dst_h) / fh - 1.0f};
  d().vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                         sizeof(rect), rect.data());
  d().vkCmdDraw(cmd, 4, 1, 0, 0);
}

const LayerCompositor::YuvProgram* LayerCompositor::GetOrCreateYuvProgram(
    VkFormat format,
    VkSamplerYcbcrModelConversion model,
    VkSamplerYcbcrRange range) {
  for (const YuvProgram& p : yuv_programs_) {
    if (p.format == format && p.model == model && p.range == range) {
      return p.ok ? &p : nullptr;  // cached success or cached failure
    }
  }

  YuvProgram prog{};
  prog.format = format;
  prog.model = model;
  prog.range = range;

  // Cache a negative result (ok stays false) and log once, so a device that
  // cannot build the program does not reattempt this every frame. Any handles
  // created before the failing step are destroyed by the caller sites below
  // before this runs, so the cached record holds only null handles.
  const auto fail = [&](const char* step) -> const YuvProgram* {
    ihs::log::warn(
        "[wl_vulkan] planar-YUV sampling unavailable ({} failed for format "
        "{:#x}); the video layer will not display -- the device likely lacks "
        "samplerYcbcrConversion or linear ycbcr filtering",
        step, static_cast<uint32_t>(format));
    yuv_programs_.push_back(prog);
    return nullptr;
  };

  // Validate the producer's colorimetry before building anything, per the "skip
  // rather than draw wrong" contract: a YUV format must carry an actual YCbCr
  // model -- not the RGB-identity value the interface defaults to, which a
  // producer that reported a YUV format but left the model unset would yield --
  // and a defined range. Bad values would otherwise build a conversion that
  // samples with unintended colorimetry. Cache the negative result and skip.
  const bool valid_model =
      model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 ||
      model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709 ||
      model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
  const bool valid_range = range == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW ||
                           range == VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
  if (!valid_model || !valid_range) {
    ihs::log::warn(
        "[wl_vulkan] planar-YUV layer for format {:#x} has invalid color "
        "model/range ({}/{}); skipping rather than sampling wrong",
        static_cast<uint32_t>(format), static_cast<uint32_t>(model),
        static_cast<uint32_t>(range));
    yuv_programs_.push_back(prog);  // cache the negative result
    return nullptr;
  }

  // The conversion that turns planar YUV texels into RGB at sample time. Chroma
  // siting is not carried by IhsFrame, so default to the H.264/HEVC 4:2:0
  // convention (horizontally cosited with luma, vertically midpoint). Linear
  // chroma reconstruction matches the sampler's linear filter, so no separate-
  // reconstruction format feature is required; a device lacking linear ycbcr
  // filtering would fail creation here and the layer is skipped.
  VkSamplerYcbcrConversionCreateInfo cci{};
  cci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
  cci.format = format;
  cci.ycbcrModel = model;
  cci.ycbcrRange = range;
  cci.components = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  cci.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
  cci.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
  cci.chromaFilter = VK_FILTER_LINEAR;
  cci.forceExplicitReconstruction = VK_FALSE;
  if (d().vkCreateSamplerYcbcrConversion(device_, &cci, nullptr,
                                         &prog.conversion) != VK_SUCCESS) {
    prog.conversion = VK_NULL_HANDLE;
    return fail("vkCreateSamplerYcbcrConversion");
  }

  // The sampler carries the conversion and is baked immutably into the set
  // layout below; matching linear filters avoid the separate-reconstruction
  // requirement, address modes are clamp (required for a ycbcr sampler).
  VkSamplerYcbcrConversionInfo conv_info{};
  conv_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
  conv_info.conversion = prog.conversion;
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.pNext = &conv_info;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (d().vkCreateSampler(device_, &si, nullptr, &prog.sampler) != VK_SUCCESS) {
    d().vkDestroySamplerYcbcrConversion(device_, prog.conversion, nullptr);
    prog.conversion = VK_NULL_HANDLE;
    prog.sampler = VK_NULL_HANDLE;
    return fail("vkCreateSampler");
  }

  // set 0, binding 0: combined image sampler with the ycbcr sampler immutable
  // (a ycbcr conversion can only be used through an immutable sampler).
  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b.pImmutableSamplers = &prog.sampler;
  VkDescriptorSetLayoutCreateInfo sl{};
  sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  sl.bindingCount = 1;
  sl.pBindings = &b;
  if (d().vkCreateDescriptorSetLayout(device_, &sl, nullptr,
                                      &prog.set_layout) != VK_SUCCESS) {
    d().vkDestroySampler(device_, prog.sampler, nullptr);
    d().vkDestroySamplerYcbcrConversion(device_, prog.conversion, nullptr);
    prog.conversion = VK_NULL_HANDLE;
    prog.sampler = VK_NULL_HANDLE;
    prog.set_layout = VK_NULL_HANDLE;
    return fail("vkCreateDescriptorSetLayout");
  }

  // Same push constant (dest rect NDC) as the RGB pipeline layout.
  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pc.offset = 0;
  pc.size = sizeof(float) * 4;
  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &prog.set_layout;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pc;
  if (d().vkCreatePipelineLayout(device_, &pl, nullptr,
                                 &prog.pipeline_layout) != VK_SUCCESS) {
    d().vkDestroyDescriptorSetLayout(device_, prog.set_layout, nullptr);
    d().vkDestroySampler(device_, prog.sampler, nullptr);
    d().vkDestroySamplerYcbcrConversion(device_, prog.conversion, nullptr);
    prog.conversion = VK_NULL_HANDLE;
    prog.sampler = VK_NULL_HANDLE;
    prog.set_layout = VK_NULL_HANDLE;
    prog.pipeline_layout = VK_NULL_HANDLE;
    return fail("vkCreatePipelineLayout");
  }

  prog.pipeline = MakePipeline(device_, render_pass_, prog.pipeline_layout);
  if (prog.pipeline == VK_NULL_HANDLE) {
    d().vkDestroyPipelineLayout(device_, prog.pipeline_layout, nullptr);
    d().vkDestroyDescriptorSetLayout(device_, prog.set_layout, nullptr);
    d().vkDestroySampler(device_, prog.sampler, nullptr);
    d().vkDestroySamplerYcbcrConversion(device_, prog.conversion, nullptr);
    prog.conversion = VK_NULL_HANDLE;
    prog.sampler = VK_NULL_HANDLE;
    prog.set_layout = VK_NULL_HANDLE;
    prog.pipeline_layout = VK_NULL_HANDLE;
    return fail("vkCreateGraphicsPipelines");
  }

  prog.ok = true;
  yuv_programs_.push_back(prog);
  return &yuv_programs_.back();
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
