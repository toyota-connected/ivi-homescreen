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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

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

// The per-draw push constants, laid out as the PC block in
// shaders/composite.frag (the vertex shader reads the first part of it).
struct LayerPush {
  // The destination quad in clip space times w, over w at its corner at
  // (s, t) = (0, 0): that corner and its edge along s, then (edge_t below) its
  // edge along t and w's steps along s and t, w being 1 at the corner. A rect
  // has its edges along the axes and w 1, which makes it the rect in NDC.
  std::array<float, 4> rect;
  // (a, c, tx, opaque) and (b, d, ty, alpha): the texture coordinates' rows,
  // then whether alpha is drawn as 1 and the view's opacity, which scales the
  // premultiplied color.
  std::array<float, 4> uv_u;
  std::array<float, 4> uv_v;
  std::array<float, 4> edge_t;  // (x, y, ws, wt)
  // How far the quad is drawn past each of its edges (s = 0, s = 1, t = 0,
  // t = 1), in s or t; one grown is smoothed. Read by the fragment shader only
  // in the masked pipeline.
  std::array<float, 4> grow;
  // The rounded clip in framebuffer pixels: its rect (left, top, right,
  // bottom), then its corner radii (x, y) upper left, upper right, lower left,
  // lower right. All radii zero is no clip.
  std::array<float, 4> rrect;
  std::array<float, 4> radii_top;
  std::array<float, 4> radii_bottom;
};
// 128 bytes is the most push constant space Vulkan guarantees.
static_assert(sizeof(LayerPush) == 128, "must match the shaders' PC block");

constexpr VkShaderStageFlags kPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// The clips a view draws as shapes, laid out as the Clips block (std140) in
// shaders/composite.frag: per clip its map's three rows, its rect, its radii.
struct ClipBlock {
  std::array<int32_t, 4> count;
  std::array<std::array<float, 4>, 3 * ClipShapes::kMax> map;
  std::array<std::array<float, 4>, ClipShapes::kMax> rect;
  std::array<std::array<float, 4>, 2 * ClipShapes::kMax> radii;
};
static_assert(sizeof(ClipBlock) == 400, "must match the shader's Clips block");

// Precompiled SPIR-V: glslc -O -mfmt=c shaders/composite.{vert,frag}, and
// composite.frag again with -DIHS_YCBCR (-o shaders/composite_ycbcr.frag.inc),
// -DIHS_CLIPS (composite_clips.frag.inc) and both (composite_ycbcr_clips).
// Regenerate with the same commands if the .glsl sources change.
constexpr uint32_t kVertSpv[] =
#include "shaders/composite.vert.inc"
    ;
constexpr uint32_t kFragSpv[] =
#include "shaders/composite.frag.inc"
    ;
constexpr uint32_t kFragYcbcrSpv[] =
#include "shaders/composite_ycbcr.frag.inc"
    ;
constexpr uint32_t kFragClipsSpv[] =
#include "shaders/composite_clips.frag.inc"
    ;
constexpr uint32_t kFragYcbcrClipsSpv[] =
#include "shaders/composite_ycbcr_clips.frag.inc"
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
// @p mask sets the fragment shader's kMask, which works out coverage (a rounded
// clip, smoothed edges); without it that code is compiled out. @p ycbcr takes
// the fragment shader built for a Y'CbCr sampler, and @p clips the one that
// draws clips as shapes (with @p mask, and a layout with their set). Returns
// VK_NULL_HANDLE on failure.
VkPipeline MakePipeline(VkDevice device,
                        VkRenderPass render_pass,
                        VkPipelineLayout layout,
                        bool mask,
                        bool ycbcr,
                        bool clips = false) {
  VkShaderModule vs = MakeModule(device, kVertSpv, sizeof(kVertSpv));
  VkShaderModule fs = VK_NULL_HANDLE;
  if (clips) {
    fs = ycbcr ? MakeModule(device, kFragYcbcrClipsSpv,
                            sizeof(kFragYcbcrClipsSpv))
               : MakeModule(device, kFragClipsSpv, sizeof(kFragClipsSpv));
  } else {
    fs = ycbcr ? MakeModule(device, kFragYcbcrSpv, sizeof(kFragYcbcrSpv))
               : MakeModule(device, kFragSpv, sizeof(kFragSpv));
  }
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  const VkBool32 mask_value = mask ? VK_TRUE : VK_FALSE;
  const VkSpecializationMapEntry mask_entry{0, 0, sizeof(VkBool32)};
  VkSpecializationInfo spec{};
  spec.mapEntryCount = 1;
  spec.pMapEntries = &mask_entry;
  spec.dataSize = sizeof(mask_value);
  spec.pData = &mask_value;
  stages[1].pSpecializationInfo = &spec;

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

std::unique_ptr<LayerCompositor> LayerCompositor::Create(
    VkDevice device,
    VkFormat color_format,
    std::string& err,
    ContentMode mode,
    VkPhysicalDevice physical) {
  std::unique_ptr<LayerCompositor> c(new LayerCompositor(device));
  c->color_format_ = color_format;

  // Render pass: one color attachment (the slot), blended into and left in
  // GENERAL for the dma-buf present.
  //
  // kClear discards prior content, so initialLayout can be UNDEFINED. kPreserve
  // keeps it, which means the layout has to be declared honestly: the content
  // being preserved was just written as a color attachment, and UNDEFINED would
  // permit the driver to throw it away -- the one thing this mode exists to
  // prevent.
  const bool preserve = mode == ContentMode::kPreserve;
  VkAttachmentDescription att{};
  att.format = color_format;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp =
      preserve ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = preserve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
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
  if (preserve) {
    // The content being loaded is the engine's render into this same image, so
    // the load has to wait on those color writes. The clear path never reads
    // prior content and so has nothing to wait for here.
    deps[0].srcStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  }
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

  // Push constants: destination rect, texture-coordinate affine and opaque
  // flag (LayerPush), read by both stages.
  VkPushConstantRange pc{};
  pc.stageFlags = kPushStages;
  pc.offset = 0;
  pc.size = sizeof(LayerPush);
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
  c->pipeline_ = MakePipeline(device, c->render_pass_, c->pipeline_layout_,
                              /*mask=*/false, /*ycbcr=*/false);
  if (c->pipeline_ == VK_NULL_HANDLE) {
    err = "vkCreateGraphicsPipelines failed";
    return nullptr;
  }
  // The same for a layer with a rounded clip. Not fatal: without it such a
  // layer is drawn with square corners.
  c->pipeline_mask_ =
      MakePipeline(device, c->render_pass_, c->pipeline_layout_, /*mask=*/true,
                   /*ycbcr=*/false);

  // One descriptor pool per ring slot, recycled in BeginFrame: a layer's
  // source, and a view's clips where it draws them as shapes.
  for (auto& fr : c->ring_) {
    std::array<VkDescriptorPoolSize, 2> ps{};
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[0].descriptorCount = kMaxLayersPerFrame;
    ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[1].descriptorCount = kMaxLayersPerFrame;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2 * kMaxLayersPerFrame;
    pi.poolSizeCount = static_cast<uint32_t>(ps.size());
    pi.pPoolSizes = ps.data();
    if (d().vkCreateDescriptorPool(device, &pi, nullptr, &fr.pool) !=
        VK_SUCCESS) {
      err = "vkCreateDescriptorPool failed";
      return nullptr;
    }
  }
  // Not fatal: without them, clips that need shapes are drawn as before.
  if (physical != VK_NULL_HANDLE) {
    c->BuildClips(physical);
  }
  return c;
}

bool LayerCompositor::BuildClips(VkPhysicalDevice physical) {
  VkPhysicalDeviceProperties props{};
  d().vkGetPhysicalDeviceProperties(physical, &props);
  const VkDeviceSize align =
      std::max<VkDeviceSize>(1, props.limits.minUniformBufferOffsetAlignment);
  clip_stride_ = (sizeof(ClipBlock) + align - 1) / align * align;
  VkPhysicalDeviceMemoryProperties mem{};
  d().vkGetPhysicalDeviceMemoryProperties(physical, &mem);

  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo sl{};
  sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  sl.bindingCount = 1;
  sl.pBindings = &b;
  if (d().vkCreateDescriptorSetLayout(device_, &sl, nullptr,
                                      &clip_set_layout_) != VK_SUCCESS) {
    clip_set_layout_ = VK_NULL_HANDLE;
    return false;
  }
  pipeline_layout_clips_ = MakeClipsLayout(set_layout_);
  if (pipeline_layout_clips_ == VK_NULL_HANDLE) {
    return false;
  }
  for (auto& fr : ring_) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = clip_stride_ * kMaxLayersPerFrame;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d().vkCreateBuffer(device_, &bi, nullptr, &fr.clips) != VK_SUCCESS) {
      fr.clips = VK_NULL_HANDLE;
      return false;
    }
    VkMemoryRequirements req{};
    d().vkGetBufferMemoryRequirements(device_, fr.clips, &req);
    constexpr VkMemoryPropertyFlags kWant =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1U << i)) != 0 &&
          (mem.memoryTypes[i].propertyFlags & kWant) == kWant) {
        type = i;
        break;
      }
    }
    if (type == UINT32_MAX) {
      return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (d().vkAllocateMemory(device_, &ai, nullptr, &fr.clips_mem) !=
        VK_SUCCESS) {
      fr.clips_mem = VK_NULL_HANDLE;
      return false;
    }
    if (d().vkBindBufferMemory(device_, fr.clips, fr.clips_mem, 0) !=
            VK_SUCCESS ||
        d().vkMapMemory(device_, fr.clips_mem, 0, VK_WHOLE_SIZE, 0,
                        &fr.clips_ptr) != VK_SUCCESS) {
      fr.clips_ptr = nullptr;
      return false;
    }
  }
  pipeline_clips_ =
      MakePipeline(device_, render_pass_, pipeline_layout_clips_,
                   /*mask=*/true, /*ycbcr=*/false, /*clips=*/true);
  return pipeline_clips_ != VK_NULL_HANDLE;
}

VkPipelineLayout LayerCompositor::MakeClipsLayout(
    VkDescriptorSetLayout source) const {
  const std::array<VkDescriptorSetLayout, 2> sets = {source, clip_set_layout_};
  VkPushConstantRange pc{};
  pc.stageFlags = kPushStages;
  pc.offset = 0;
  pc.size = sizeof(LayerPush);
  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = static_cast<uint32_t>(sets.size());
  pl.pSetLayouts = sets.data();
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pc;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (d().vkCreatePipelineLayout(device_, &pl, nullptr, &layout) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return layout;
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
    if (fr.clips != VK_NULL_HANDLE) {
      d().vkDestroyBuffer(device_, fr.clips, nullptr);
    }
    if (fr.clips_mem != VK_NULL_HANDLE) {
      d().vkFreeMemory(device_, fr.clips_mem, nullptr);  // unmaps it
    }
  }
  for (const YuvProgram& p : yuv_programs_) {
    // A cached negative result (ok == false) holds only null handles; guard
    // each destroy like the rest of this destructor rather than relying on
    // Vulkan accepting null.
    if (p.pipeline != VK_NULL_HANDLE) {
      d().vkDestroyPipeline(device_, p.pipeline, nullptr);
    }
    if (p.pipeline_mask != VK_NULL_HANDLE) {
      d().vkDestroyPipeline(device_, p.pipeline_mask, nullptr);
    }
    if (p.pipeline_clips != VK_NULL_HANDLE) {
      d().vkDestroyPipeline(device_, p.pipeline_clips, nullptr);
    }
    if (p.pipeline_layout_clips != VK_NULL_HANDLE) {
      d().vkDestroyPipelineLayout(device_, p.pipeline_layout_clips, nullptr);
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
  if (pipeline_mask_ != VK_NULL_HANDLE) {
    d().vkDestroyPipeline(device_, pipeline_mask_, nullptr);
  }
  if (pipeline_clips_ != VK_NULL_HANDLE) {
    d().vkDestroyPipeline(device_, pipeline_clips_, nullptr);
  }
  if (pipeline_layout_clips_ != VK_NULL_HANDLE) {
    d().vkDestroyPipelineLayout(device_, pipeline_layout_clips_, nullptr);
  }
  if (clip_set_layout_ != VK_NULL_HANDLE) {
    d().vkDestroyDescriptorSetLayout(device_, clip_set_layout_, nullptr);
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
  fr.clips_used = 0;
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
                                int32_t dst_h,
                                const UvAffine& uv,
                                bool opaque,
                                float alpha,
                                const RoundedRect* rounded,
                                const Quad* quad,
                                const ClipShapes* shapes) {
  if (fb_width_ == 0 || fb_height_ == 0) {
    return;
  }
  auto& fr = ring_[cur_ring_];

  // RGB layers use the shared pipeline/sampler; a YUV layer resolves (or
  // builds) the program whose immutable ycbcr sampler bakes the conversion, and
  // its view carries the same conversion. With an immutable sampler the
  // descriptor write leaves the sampler null (the layout supplies it).
  // A rounded clip takes the pipeline built to cut corners; one that could not
  // be built leaves them square.
  const RoundedRect* clip = rounded;
  if (clip != nullptr && pipeline_mask_ == VK_NULL_HANDLE) {
    WarnNoMask();
    clip = nullptr;
  }
  VkPipeline pipeline = pipeline_;
  VkPipeline pipeline_mask = pipeline_mask_;
  VkPipeline pipeline_clips = pipeline_clips_;
  VkPipelineLayout pipeline_layout = pipeline_layout_;
  VkPipelineLayout pipeline_layout_clips = pipeline_layout_clips_;
  VkDescriptorSetLayout set_layout = set_layout_;
  VkSampler sampler = sampler_;
  VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
  if (IsYuvFormat(src_format)) {
    const YuvProgram* prog =
        GetOrCreateYuvProgram(src_format, ycbcr_model, ycbcr_range);
    if (prog == nullptr) {
      return;  // conversion/pipeline unavailable — skip rather than draw wrong
    }
    if (clip != nullptr && prog->pipeline_mask == VK_NULL_HANDLE) {
      WarnNoMask();
      clip = nullptr;
    }
    pipeline = prog->pipeline;
    pipeline_mask = prog->pipeline_mask;
    pipeline_clips = prog->pipeline_clips;
    pipeline_layout = prog->pipeline_layout;
    pipeline_layout_clips = prog->pipeline_layout_clips;
    set_layout = prog->set_layout;
    sampler = VK_NULL_HANDLE;  // immutable in the layout
    conversion = prog->conversion;
  }

  // Clips drawn as shapes take the place of the rounded clip, through the
  // pipeline built for them and the next slot of this frame's buffer. Where
  // that cannot be had, the rounded clip is drawn as before.
  const bool use_shapes =
      shapes != nullptr && shapes->count > 0 &&
      shapes->count <= ClipShapes::kMax && pipeline_clips != VK_NULL_HANDLE &&
      fr.clips_ptr != nullptr && fr.clips_used < kMaxLayersPerFrame;
  if (shapes != nullptr && shapes->count > 0 && !use_shapes) {
    WarnNoClips();
  }
  if (use_shapes) {
    clip = nullptr;
    pipeline_mask = pipeline_clips;
    pipeline_layout = pipeline_layout_clips;
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
  std::array<VkDescriptorSet, 2> sets = {set, VK_NULL_HANDLE};
  if (use_shapes) {
    const VkDeviceSize offset = clip_stride_ * fr.clips_used;
    ClipBlock block{};
    block.count = {static_cast<int32_t>(shapes->count), 0, 0, 0};
    for (size_t k = 0; k < shapes->count; ++k) {
      const ClipShape& c = shapes->items[k];
      const ViewMap& m = c.to_shape;
      // A pixel's span in the shape's space, padded: the shader takes a
      // point that far inside the straight parts as wholly inside.
      const double unit = std::min(1.5 * ClipShapeUnit(c), 1e30);
      block.map[3 * k] = {static_cast<float>(m.a), static_cast<float>(m.c),
                          static_cast<float>(m.tx), static_cast<float>(unit)};
      block.map[3 * k + 1] = {static_cast<float>(m.b), static_cast<float>(m.d),
                              static_cast<float>(m.ty), 0.0f};
      block.map[3 * k + 2] = {static_cast<float>(m.g), static_cast<float>(m.h),
                              static_cast<float>(m.i), 0.0f};
      const RectF& b = c.shape.rect;
      block.rect[k] = {static_cast<float>(b.x), static_cast<float>(b.y),
                       static_cast<float>(b.x + b.w),
                       static_cast<float>(b.y + b.h)};
      const auto& r = c.shape.radii;
      block.radii[2 * k] = {r[0][0], r[0][1], r[1][0], r[1][1]};
      block.radii[2 * k + 1] = {r[3][0], r[3][1], r[2][0], r[2][1]};
    }
    std::memcpy(static_cast<uint8_t*>(fr.clips_ptr) + offset, &block,
                sizeof(block));
    VkDescriptorSetAllocateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ci.descriptorPool = fr.pool;
    ci.descriptorSetCount = 1;
    ci.pSetLayouts = &clip_set_layout_;
    if (d().vkAllocateDescriptorSets(device_, &ci, &sets[1]) != VK_SUCCESS) {
      return;
    }
    ++fr.clips_used;
    VkDescriptorBufferInfo bi{};
    bi.buffer = fr.clips;
    bi.offset = offset;
    bi.range = sizeof(ClipBlock);
    VkWriteDescriptorSet cw{};
    cw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cw.dstSet = sets[1];
    cw.dstBinding = 0;
    cw.descriptorCount = 1;
    cw.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cw.pBufferInfo = &bi;
    d().vkUpdateDescriptorSets(device_, 1, &cw, 0, nullptr);
  }
  d().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_layout, 0, use_shapes ? 2U : 1U,
                              sets.data(), 0, nullptr);

  const auto fw = static_cast<float>(fb_width_);
  const auto fh = static_cast<float>(fb_height_);
  const RectI whole{dst_x, dst_y, dst_w, dst_h};
  LayerPush push{};
  const float opaque_f = opaque ? 1.0f : 0.0f;
  if (clip != nullptr) {
    const RectF& b = clip->rect;
    const auto& r = clip->radii;
    push.rrect = {static_cast<float>(b.x), static_cast<float>(b.y),
                  static_cast<float>(b.x + b.w), static_cast<float>(b.y + b.h)};
    push.radii_top = {r[0][0], r[0][1], r[1][0], r[1][1]};
    push.radii_bottom = {r[3][0], r[3][1], r[2][0], r[2][1]};
  }
  // Draws the part @p sub of the destination rect. Without the mask the
  // pipeline reads none of the clip's fields, so only the part before them is
  // pushed.
  VkPipeline bound = VK_NULL_HANDLE;
  const auto draw = [&](const RectI& sub, const bool masked) {
    VkPipeline want = masked ? pipeline_mask : pipeline;
    if (want != bound) {
      d().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
      bound = want;
    }
    // Destination rect -> NDC (top-left origin, y down; matches the blit
    // path).
    const float x0 = 2.0f * static_cast<float>(sub.x) / fw - 1.0f;
    const float y0 = 2.0f * static_cast<float>(sub.y) / fh - 1.0f;
    push.rect = {x0, y0,
                 2.0f * static_cast<float>(sub.x + sub.w) / fw - 1.0f - x0,
                 0.0f};
    push.edge_t = {0.0f,
                   2.0f * static_cast<float>(sub.y + sub.h) / fh - 1.0f - y0,
                   0.0f, 0.0f};
    const UvAffine u = SubUv(uv, whole, sub);
    push.uv_u = {u.a, u.c, u.tx, opaque_f};
    push.uv_v = {u.b, u.d, u.ty, alpha};
    d().vkCmdPushConstants(cmd, pipeline_layout, kPushStages, 0,
                           masked ? sizeof(push) : offsetof(LayerPush, rrect),
                           &push);
    d().vkCmdDraw(cmd, 4, 1, 0, 0);
  };
  // A layer of a transformed view is its quad, clipped by the scissor; a
  // rounded clip and smoothed edges are worked out over all of it. Without the
  // masked pipeline its edges are left hard. The scissor goes back to the
  // whole framebuffer after, as BeginFrame left it.
  if (quad != nullptr) {
    const int32_t sx0 = std::max(0, quad->scissor.x);
    const int32_t sy0 = std::max(0, quad->scissor.y);
    const int32_t sx1 = std::min(static_cast<int32_t>(fb_width_),
                                 quad->scissor.x + quad->scissor.w);
    const int32_t sy1 = std::min(static_cast<int32_t>(fb_height_),
                                 quad->scissor.y + quad->scissor.h);
    if (sx1 <= sx0 || sy1 <= sy0) {
      return;
    }
    const VkRect2D cut{
        {sx0, sy0},
        {static_cast<uint32_t>(sx1 - sx0), static_cast<uint32_t>(sy1 - sy0)}};
    d().vkCmdSetScissor(cmd, 0, 1, &cut);
    const bool smooth = pipeline_mask != VK_NULL_HANDLE &&
                        (quad->grow[0] > 0 || quad->grow[1] > 0 ||
                         quad->grow[2] > 0 || quad->grow[3] > 0);
    const bool masked = clip != nullptr || smooth || use_shapes;
    VkPipeline want = masked ? pipeline_mask : pipeline;
    d().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
    // Pixels times w into clip space times w: x * w maps to
    // (2 x / fw - 1) * w, affine in (s, t) like x * w itself. All of it over
    // the corner at (0, 0)'s w, which leaves the picture as it is and keeps
    // the numbers near 1.
    const double k = 1.0 / quad->w0;
    const auto to_clip = [k](const double v, const double size,
                             const double w) {
      return static_cast<float>((2.0 * v / size - w) * k);
    };
    push.rect = {to_clip(quad->origin[0], fw, quad->w0),
                 to_clip(quad->origin[1], fh, quad->w0),
                 to_clip(quad->edge_s[0], fw, quad->ws),
                 to_clip(quad->edge_s[1], fh, quad->ws)};
    push.edge_t = {to_clip(quad->edge_t[0], fw, quad->wt),
                   to_clip(quad->edge_t[1], fh, quad->wt),
                   static_cast<float>(quad->ws * k),
                   static_cast<float>(quad->wt * k)};
    if (smooth) {
      for (size_t i = 0; i < push.grow.size(); ++i) {
        push.grow[i] = static_cast<float>(quad->grow[i]);
      }
    }
    push.uv_u = {uv.a, uv.c, uv.tx, opaque_f};
    push.uv_v = {uv.b, uv.d, uv.ty, alpha};
    d().vkCmdPushConstants(cmd, pipeline_layout, kPushStages, 0,
                           masked ? sizeof(push) : offsetof(LayerPush, rrect),
                           &push);
    // Clips drawn as shapes cost every pixel they are worked out at, so the
    // quad's bounds are cut into pieces (ClipPieces): those inside every clip
    // and the quad are drawn with the plain pipeline, which needs neither,
    // those near an edge with the clips, and those clearly outside not at
    // all. Each is the same quad, cut by its own scissor.
    if (use_shapes) {
      QuadPlacement placed;
      placed.origin = quad->origin;
      placed.edge_s = quad->edge_s;
      placed.edge_t = quad->edge_t;
      placed.w0 = quad->w0;
      placed.ws = quad->ws;
      placed.wt = quad->wt;
      VkPipeline bound_piece = want;
      for (const ClipPiece& piece : piece_cache_.Get(
               *shapes, &placed, {sx0, sy0, sx1 - sx0, sy1 - sy0})) {
        const RectI& r = piece.rect;
        const VkRect2D cut_piece{
            {r.x, r.y},
            {static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h)}};
        d().vkCmdSetScissor(cmd, 0, 1, &cut_piece);
        VkPipeline p = piece.mask ? want : pipeline;
        if (p != bound_piece) {
          d().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
          bound_piece = p;
        }
        d().vkCmdDraw(cmd, 4, 1, 0, 0);
      }
    } else {
      d().vkCmdDraw(cmd, 4, 1, 0, 0);
    }
    const VkRect2D all{{0, 0}, {fb_width_, fb_height_}};
    d().vkCmdSetScissor(cmd, 0, 1, &all);
    return;
  }
  // Only the boxes around a rounded clip's corners take the mask, which costs
  // every pixel it covers; the rest draws with the plain pipeline.
  if (clip == nullptr && use_shapes) {
    for (const ClipPiece& piece : piece_cache_.Get(*shapes, nullptr, whole)) {
      draw(piece.rect, piece.mask);
    }
    return;
  }
  if (clip == nullptr) {
    draw(whole, false);
    return;
  }
  std::array<RoundedPiece, 7> pieces;
  const size_t n = SplitRoundedCorners(whole, *clip, pieces);
  for (size_t k = 0; k < n; ++k) {
    draw(pieces[k].rect, pieces[k].corner);
  }
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

  // Same push constants (LayerPush) as the RGB pipeline layout.
  VkPushConstantRange pc{};
  pc.stageFlags = kPushStages;
  pc.offset = 0;
  pc.size = sizeof(LayerPush);
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

  prog.pipeline = MakePipeline(device_, render_pass_, prog.pipeline_layout,
                               /*mask=*/false, /*ycbcr=*/true);
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

  // As for RGB: not fatal, a rounded clip is then drawn square.
  prog.pipeline_mask = MakePipeline(device_, render_pass_, prog.pipeline_layout,
                                    /*mask=*/true, /*ycbcr=*/true);
  // And clips that need shapes are drawn as their bounding boxes.
  if (clip_set_layout_ != VK_NULL_HANDLE && pipeline_clips_ != VK_NULL_HANDLE) {
    prog.pipeline_layout_clips = MakeClipsLayout(prog.set_layout);
    if (prog.pipeline_layout_clips != VK_NULL_HANDLE) {
      prog.pipeline_clips =
          MakePipeline(device_, render_pass_, prog.pipeline_layout_clips,
                       /*mask=*/true, /*ycbcr=*/true, /*clips=*/true);
    }
  }
  prog.ok = true;
  yuv_programs_.push_back(prog);
  return &yuv_programs_.back();
}

void LayerCompositor::WarnNoClips() {
  if (warned_no_clips_) {
    return;
  }
  warned_no_clips_ = true;
  ihs::log::warn(
      "[wl_vulkan] a platform view's clips are not rects on screen, and the "
      "pipeline that draws them as shapes is not there; they are drawn as "
      "their bounding boxes");
}

void LayerCompositor::WarnNoMask() {
  if (warned_no_mask_) {
    return;
  }
  warned_no_mask_ = true;
  ihs::log::warn(
      "[wl_vulkan] the rounded-clip pipeline could not be built; platform "
      "views "
      "with rounded corners are drawn square");
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
