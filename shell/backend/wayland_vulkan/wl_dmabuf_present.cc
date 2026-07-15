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

#include "backend/wayland_vulkan/wl_dmabuf_present.h"

#include <unistd.h>
#include <array>
#include <set>

#include <drm_fourcc.h>

// Dynamic dispatch, matching the backend (which owns the loader storage and
// calls VULKAN_HPP_DEFAULT_DISPATCHER.init()); do NOT define the storage here.
#define VULKAN_HPP_NO_EXCEPTIONS 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

// clang-format off
// Order is load-bearing: the generated client headers define the traits the
// shipped wl/ helper below references. clang-format must not reorder these.
#include "linux_dmabuf_client.hpp"  // linux_dmabuf_unstable_v1::client + traits
#include "wayland_client.hpp"       // wayland::client::CWlBuffer, wl_buffer_traits
#include <wl/wayland.hpp>           // core wl_iface() impls (wl_buffer, ...)
#include <wl/linux_dmabuf.hpp>      // shipped tables (must follow the generated)
// clang-format on
#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/proxy_impl.hpp>      // wl::construct
#include <wl/wl_ptr.hpp>          // wl::WlPtr

// wl_buffer is a core object. ivi generates the core protocol without interface
// tables (they come from libwayland), and no other translation unit has needed
// a core object's wl_iface() before, so the shipped wl/ headers do not define
// this one. Provide it — it simply maps to libwayland's wl_buffer_interface —
// so wl::construct<wl_buffer_traits> can create the dma-buf wl_buffer.
namespace wayland::client {
const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}
}  // namespace wayland::client

namespace wl_vulkan {

namespace {

const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

uint32_t PickMemoryType(VkPhysicalDevice phys,
                        uint32_t type_bits,
                        VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  d().vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & want) == want) {
      return i;
    }
  }
  return UINT32_MAX;
}

// Plane count the driver uses for @p modifier with @p vk_format.
uint32_t PlaneCountForModifier(VkPhysicalDevice phys,
                               VkFormat vk_format,
                               uint64_t modifier) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  d().vkGetPhysicalDeviceFormatProperties2(phys, vk_format, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  d().vkGetPhysicalDeviceFormatProperties2(phys, vk_format, &fp);
  for (const auto& m : mods) {
    if (m.drmFormatModifier == modifier) {
      return m.drmFormatModifierPlaneCount;
    }
  }
  return 0;
}

}  // namespace

std::vector<uint64_t> NegotiateModifiers(
    VkPhysicalDevice physical_device,
    VkFormat vk_format,
    const std::vector<uint64_t>& compositor_modifiers) {
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  d().vkGetPhysicalDeviceFormatProperties2(physical_device, vk_format, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  d().vkGetPhysicalDeviceFormatProperties2(physical_device, vk_format, &fp);

  // Renderable + blit-target + sampleable: the compositor path blits the
  // engine's frame into this image, then the compositor samples/scans it out.
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  std::set<uint64_t> exportable;
  for (const auto& m : mods) {
    if ((m.drmFormatModifierTilingFeatures & need) == need) {
      exportable.insert(m.drmFormatModifier);
    }
  }

  const std::set<uint64_t> comp(compositor_modifiers.begin(),
                                compositor_modifiers.end());
  std::vector<uint64_t> tiled;
  bool has_linear = false;
  for (uint64_t m : exportable) {
    if (!comp.count(m)) {
      continue;
    }
    if (m == DRM_FORMAT_MOD_LINEAR) {
      has_linear = true;
    } else {
      tiled.push_back(m);
    }
  }
  if (has_linear) {
    tiled.push_back(DRM_FORMAT_MOD_LINEAR);
  }
  return tiled;
}

std::unique_ptr<DmabufImage> DmabufImage::Create(
    VkPhysicalDevice physical_device,
    VkDevice device,
    uint32_t width,
    uint32_t height,
    VkFormat vk_format,
    uint32_t drm_fourcc,
    VkImageUsageFlags usage,
    const std::vector<uint64_t>& allowed_modifiers,
    std::string& err) {
  if (allowed_modifiers.empty()) {
    err = "no allowed modifiers (empty negotiation result)";
    return nullptr;
  }

  std::unique_ptr<DmabufImage> img(new DmabufImage(device, width, height));
  img->drm_fourcc_ = drm_fourcc;

  VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
  mod_list.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  mod_list.drmFormatModifierCount =
      static_cast<uint32_t>(allowed_modifiers.size());
  mod_list.pDrmFormatModifiers = allowed_modifiers.data();
  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ext.pNext = &mod_list;

  VkImageCreateInfo ic{};
  ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ic.pNext = &ext;
  ic.imageType = VK_IMAGE_TYPE_2D;
  ic.format = vk_format;
  ic.extent = {width, height, 1};
  ic.mipLevels = 1;
  ic.arrayLayers = 1;
  ic.samples = VK_SAMPLE_COUNT_1_BIT;
  ic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ic.usage = usage;
  ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (d().vkCreateImage(device, &ic, nullptr, &img->image_) != VK_SUCCESS) {
    err = "vkCreateImage with modifier list failed";
    return nullptr;
  }

  VkImageDrmFormatModifierPropertiesEXT mp{};
  mp.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
  if (d().vkGetImageDrmFormatModifierPropertiesEXT(device, img->image_, &mp) !=
      VK_SUCCESS) {
    err = "vkGetImageDrmFormatModifierPropertiesEXT failed";
    return nullptr;
  }
  img->modifier_ = mp.drmFormatModifier;

  const uint32_t plane_count =
      PlaneCountForModifier(physical_device, vk_format, img->modifier_);
  if (plane_count < 1 || plane_count > 4) {
    err = "unexpected plane count for chosen modifier";
    return nullptr;
  }

  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device, img->image_, &req);
  const uint32_t mt = PickMemoryType(physical_device, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt == UINT32_MAX) {
    err = "no DEVICE_LOCAL memory type for scanout image";
    return nullptr;
  }
  // Dedicated allocation: many drivers require (and all allow) a dma-buf
  // exported image's memory to be bound 1:1 to that image. Without it
  // vkBindImageMemory fails on Mesa/RADV and others.
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = img->image_;
  VkExportMemoryAllocateInfo emai{};
  emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  emai.pNext = &dedicated;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &emai;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (d().vkAllocateMemory(device, &mai, nullptr, &img->memory_) !=
      VK_SUCCESS) {
    err = "vkAllocateMemory (exported) failed";
    return nullptr;
  }
  if (d().vkBindImageMemory(device, img->image_, img->memory_, 0) !=
      VK_SUCCESS) {
    err = "vkBindImageMemory failed";
    return nullptr;
  }

  static constexpr std::array<VkImageAspectFlagBits, 4> kMemPlane = {
      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
  };
  img->planes_.resize(plane_count);
  for (uint32_t i = 0; i < plane_count; ++i) {
    VkImageSubresource sub{};
    sub.aspectMask = kMemPlane[i];
    VkSubresourceLayout sl{};
    d().vkGetImageSubresourceLayout(device, img->image_, &sub, &sl);
    img->planes_[i] = {sl.offset, sl.rowPitch};
  }

  VkMemoryGetFdInfoKHR gfi{};
  gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  gfi.memory = img->memory_;
  gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if (d().vkGetMemoryFdKHR(device, &gfi, &img->dma_buf_fd_) != VK_SUCCESS) {
    err = "vkGetMemoryFdKHR failed";
    return nullptr;
  }

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = img->image_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = vk_format;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (d().vkCreateImageView(device, &vci, nullptr, &img->view_) != VK_SUCCESS) {
    err = "vkCreateImageView failed";
    return nullptr;
  }

  return img;
}

DmabufImage::~DmabufImage() {
  if (view_ != VK_NULL_HANDLE) {
    d().vkDestroyImageView(device_, view_, nullptr);
  }
  if (image_ != VK_NULL_HANDLE) {
    d().vkDestroyImage(device_, image_, nullptr);
  }
  if (memory_ != VK_NULL_HANDLE) {
    d().vkFreeMemory(device_, memory_, nullptr);
  }
  if (dma_buf_fd_ >= 0) {
    close(dma_buf_fd_);
  }
}

namespace {

// Tracks wl_buffer.release; released_ starts true (compositor does not hold
// it).
class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool released_ = true;
  void OnRelease() override { released_ = true; }
};

// create_immed is synchronous — no created/failed events to handle.
class LinuxBufferParamsHandler
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxBufferParamsV1<
          LinuxBufferParamsHandler> {};

}  // namespace

struct WlDmabufBuffer::Impl {
  wl::WlPtr<WlBufferHandler> buffer;
};

WlDmabufBuffer::WlDmabufBuffer() : impl_(std::make_unique<Impl>()) {}
WlDmabufBuffer::~WlDmabufBuffer() = default;

std::unique_ptr<WlDmabufBuffer> WlDmabufBuffer::Create(wl_proxy* params_proxy,
                                                       const DmabufImage& img) {
  if (params_proxy == nullptr) {
    return nullptr;
  }
  wl::WlPtr<LinuxBufferParamsHandler> params;
  params.Attach(params_proxy);

  // One add() per memory plane, all sharing the fd (libwayland dups it).
  const uint64_t mod = img.modifier();
  const auto& planes = img.planes();
  for (uint32_t i = 0; i < planes.size(); ++i) {
    params.Get()->Add(img.dma_buf_fd(), i,
                      static_cast<uint32_t>(planes[i].offset),
                      static_cast<uint32_t>(planes[i].pitch),
                      static_cast<uint32_t>(mod >> 32u),
                      static_cast<uint32_t>(mod & 0xffffffffu));
  }

  wl_proxy* raw =
      wl::construct<wayland::client::wl_buffer_traits,
                    linux_dmabuf_unstable_v1::client::
                        zwp_linux_buffer_params_v1_traits::Op::CreateImmed>(
          *params.Get(), static_cast<int32_t>(img.width()),
          static_cast<int32_t>(img.height()), img.drm_fourcc(), 0u);
  if (raw == nullptr) {
    return nullptr;
  }

  std::unique_ptr<WlDmabufBuffer> out(new WlDmabufBuffer());
  out->impl_->buffer.Get()->_SetProxy(raw);
  out->impl_->buffer.Get()->released_ = true;
  return out;
}

wl_proxy* WlDmabufBuffer::proxy() const {
  return impl_->buffer.Get()->GetProxy();
}

bool WlDmabufBuffer::released() const {
  return impl_->buffer.Get()->released_;
}

void WlDmabufBuffer::mark_in_use() {
  impl_->buffer.Get()->released_ = false;
}

}  // namespace wl_vulkan
