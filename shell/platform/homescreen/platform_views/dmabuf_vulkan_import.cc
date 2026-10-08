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

#include "dmabuf_vulkan_import.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <mutex>

#include "logging/logging.h"

namespace {

// V3D_TFU_READAHEAD_SIZE in Mesa's v3dv (src/broadcom/vulkan/v3dv_private.h).
constexpr VkDeviceSize kV3dvTfuReadahead = 64;

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// Maps a DRM fourcc to the Vulkan format an imported dma-buf is created with.
// The packed RGB formats are sampled through a plain B8G8R8A8-style view; the
// planar ones through a VkSamplerYcbcrConversion the compositor builds.
// `is_yuv` distinguishes the two so the caller knows which path an image needs.
VkFormat VkFormatFromFourcc(uint32_t fourcc, bool* is_yuv) {
  *is_yuv = false;
  if (fourcc == Fourcc('A', 'R', '2', '4') ||  // DRM_FORMAT_ARGB8888
      fourcc == Fourcc('X', 'R', '2', '4')) {  // DRM_FORMAT_XRGB8888
    return VK_FORMAT_B8G8R8A8_UNORM;
  }
  if (fourcc == Fourcc('A', 'B', '2', '4') ||  // DRM_FORMAT_ABGR8888
      fourcc == Fourcc('X', 'B', '2', '4')) {  // DRM_FORMAT_XBGR8888
    return VK_FORMAT_R8G8B8A8_UNORM;
  }
  // NV12: Y plane then interleaved CbCr, 4:2:0. What the VAAPI and V4L2
  // decoders both emit. Two planes on one dma-buf, so a non-disjoint 2-plane
  // image; the plane offsets come from the frame.
  if (fourcc == Fourcc('N', 'V', '1', '2')) {  // DRM_FORMAT_NV12
    *is_yuv = true;
    return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

// The YCbCr->RGB conversion the compositor's sampler must apply, resolved from
// the frame's color metadata. IHS_COLOR_SPACE_DEFAULT follows the contract in
// platform_view.h -- BT.601 for SD, BT.709 for HD, split at the standard 720p
// boundary -- and IHS_COLOR_RANGE_DEFAULT is studio (narrow) range, what the
// hardware decoders emit. This is the first shell implementation of that
// default; the EGL path leaves the same choice to the GL driver.
void ResolveYcbcr(const IhsFrame& frame,
                  VkSamplerYcbcrModelConversion* model,
                  VkSamplerYcbcrRange* range) {
  switch (frame.color_space) {
    case IHS_COLOR_SPACE_BT601:
      *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
      break;
    case IHS_COLOR_SPACE_BT709:
      *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
      break;
    case IHS_COLOR_SPACE_BT2020:
      *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
      break;
    default:  // IHS_COLOR_SPACE_DEFAULT
      *model = frame.height >= 720
                   ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
                   : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
      break;
  }
  *range = frame.color_range == IHS_COLOR_RANGE_FULL
               ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
               : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
}

// The first memory type common to the image's requirements and the imported
// fd's properties, or UINT32_MAX. dma-buf memory need not be DEVICE_LOCAL, so
// intersect the two masks rather than demanding a property.
uint32_t PickMemoryType(const VkPhysicalDeviceMemoryProperties& props,
                        uint32_t allowed) {
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((allowed & (1u << i)) != 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

}  // namespace

bool DmabufVulkanImporter::Init(VkInstance instance,
                                VkPhysicalDevice physical_device,
                                VkDevice device,
                                void* get_instance_proc_addr) {
  if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE ||
      device == VK_NULL_HANDLE || get_instance_proc_addr == nullptr) {
    return false;
  }
  auto gipa =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
  auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      gipa(instance, "vkGetDeviceProcAddr"));
  if (get_device_proc_addr == nullptr) {
    return false;
  }

  auto instance_fn = [&](const char* name) { return gipa(instance, name); };
  auto device_fn = [&](const char* name) {
    return get_device_proc_addr(device, name);
  };

  get_memory_properties_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          instance_fn("vkGetPhysicalDeviceMemoryProperties"));
  create_image_ =
      reinterpret_cast<PFN_vkCreateImage>(device_fn("vkCreateImage"));
  destroy_image_ =
      reinterpret_cast<PFN_vkDestroyImage>(device_fn("vkDestroyImage"));
  get_image_memory_requirements_ =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
          device_fn("vkGetImageMemoryRequirements"));
  allocate_memory_ =
      reinterpret_cast<PFN_vkAllocateMemory>(device_fn("vkAllocateMemory"));
  free_memory_ = reinterpret_cast<PFN_vkFreeMemory>(device_fn("vkFreeMemory"));
  bind_image_memory_ =
      reinterpret_cast<PFN_vkBindImageMemory>(device_fn("vkBindImageMemory"));
  get_memory_fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      device_fn("vkGetMemoryFdPropertiesKHR"));
  get_image_subresource_layout_ =
      reinterpret_cast<PFN_vkGetImageSubresourceLayout>(
          device_fn("vkGetImageSubresourceLayout"));
  // Optional: only ImportableModifiers uses these, and it degrades to "ask
  // nothing, offer what we always did" without them.
  get_format_properties2_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
          instance_fn("vkGetPhysicalDeviceFormatProperties2"));
  get_image_format_properties2_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
          instance_fn("vkGetPhysicalDeviceImageFormatProperties2"));

  if (get_memory_properties_ == nullptr || create_image_ == nullptr ||
      destroy_image_ == nullptr || get_image_memory_requirements_ == nullptr ||
      allocate_memory_ == nullptr || free_memory_ == nullptr ||
      bind_image_memory_ == nullptr || get_memory_fd_properties_ == nullptr ||
      get_image_subresource_layout_ == nullptr) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import unavailable: the backend's Vulkan device is "
        "missing an external-memory-fd entry point");
    return false;
  }

  // v3dv charges an imported dma-buf for its TFU read-ahead
  // (V3D_TFU_READAHEAD_SIZE, 64 bytes) and then rounds to a page, so a dma-buf
  // sized exactly to its image -- what the v3d GL allocator hands a client --
  // is a page short and the import fails with
  // VK_ERROR_INVALID_EXTERNAL_HANDLE (#691, mesa/mesa#16524). Where the 64
  // bytes are added moved between versions: Mesa 25.0.7 adds them in
  // v3dv_AllocateMemory, main reports them inside
  // VkMemoryRequirements::size and rounds that alone. Either way the image
  // size alone decides whether a frame hits it -- one leaving 64 bytes spare
  // in its last page imports, one leaving fewer does not -- which is why the
  // same producer's toplevel buffers imported while its popup did not, and
  // either way subtracting the read-ahead is what cancels it out.
  if (auto get_properties2 =
          reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
              instance_fn("vkGetPhysicalDeviceProperties2"))) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    get_properties2(physical_device, &props);
    if (driver.driverID == VK_DRIVER_ID_MESA_V3DV) {
      import_padding_ = kV3dvTfuReadahead;
    }
    // VeriSilicon (by vendor: the driver reports another driver's ID) binds a
    // LINEAR dma-buf to a DRM_FORMAT_MODIFIER image without error, ignores the
    // explicit row pitch, and reads it as its own tiled layout: every frame
    // samples as noise. A LINEAR-tiled image over the same import reads
    // correctly. Measured on an i.MX8MP, XRGB8888, 3x3 through 4096x2160.
    linear_tiling_for_linear_ = props.properties.vendorID == VK_VENDOR_ID_VSI;
  }

  instance_ = instance;
  physical_device_ = physical_device;
  device_ = device;
  return true;
}

std::vector<uint64_t> DmabufVulkanImporter::ImportableModifiers(
    const uint32_t drm_fourcc) const {
  std::vector<uint64_t> out;
  if (get_format_properties2_ == nullptr ||
      get_image_format_properties2_ == nullptr ||
      physical_device_ == VK_NULL_HANDLE) {
    return out;
  }
  bool is_yuv = false;
  const VkFormat format = VkFormatFromFourcc(drm_fourcc, &is_yuv);
  if (format == VK_FORMAT_UNDEFINED) {
    return out;
  }

  // Everything the device knows about this format, then filtered by what it
  // will actually import. The tiling-feature bits alone are not the answer --
  // they say what the modifier can do, not whether a dma-buf carrying it can
  // be brought in with this usage.
  VkDrmFormatModifierPropertiesListEXT list{};
  list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
  VkFormatProperties2 fp{};
  fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
  fp.pNext = &list;
  get_format_properties2_(physical_device_, format, &fp);
  if (list.drmFormatModifierCount == 0) {
    return out;
  }
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  get_format_properties2_(physical_device_, format, &fp);

  // The memory planes Import() builds an image from: one for RGB, the two of
  // a non-disjoint NV12.
  const uint32_t planes = is_yuv ? 2U : 1U;
  for (const auto& m : mods) {
    // Exactly those planes. A modifier with a metadata plane on top (AMD DCC,
    // for one) cannot be satisfied by any producer on this ABI. The device
    // will happily report it as importable, because it is importable with the
    // right plane count; offering it hands the producer a modifier whose
    // every import then fails with
    // VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, once per frame,
    // with nothing on screen.
    if (m.drmFormatModifierPlaneCount != planes) {
      continue;
    }
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{};
    mod_info.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    mod_info.drmFormatModifier = m.drmFormatModifier;
    mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkPhysicalDeviceExternalImageFormatInfo ext_info{};
    ext_info.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    ext_info.pNext = &mod_info;
    ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkPhysicalDeviceImageFormatInfo2 fi{};
    fi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    fi.pNext = &ext_info;
    fi.format = format;
    fi.type = VK_IMAGE_TYPE_2D;
    fi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    // The same usage Import asks for. Probing with anything else answers a
    // question we are not going to ask at import time.
    fi.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkExternalImageFormatProperties ext_props{};
    ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = &ext_props;

    if (get_image_format_properties2_(physical_device_, &fi, &props) !=
        VK_SUCCESS) {
      continue;
    }
    if ((ext_props.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
      continue;
    }
    out.push_back(m.drmFormatModifier);
  }
  return out;
}

bool DmabufVulkanImporter::Import(const IhsFrame& frame,
                                  ImportedImage* out) const {
  if (!ready() || out == nullptr) {
    return false;
  }
  bool is_yuv = false;
  const VkFormat format = VkFormatFromFourcc(frame.format.fourcc, &is_yuv);
  if (format == VK_FORMAT_UNDEFINED) {
    ihs::log::warn("[ihs_pv] dma-buf import: unsupported fourcc {:#x}",
                   frame.format.fourcc);
    return false;
  }

  // The plane count is fixed by the format: 1 for the packed RGB formats, 2 for
  // NV12. Reject a frame whose plane_count disagrees so an RGB fourcc cannot
  // arrive claiming two planes (which would mis-drive the plane-layout array
  // and vkCreateImage) and NV12 cannot arrive under-specified.
  const uint32_t expected_planes = is_yuv ? 2u : 1u;
  if (frame.plane_count != expected_planes) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import: fourcc {:#x} expects {} plane(s), got {}",
        frame.format.fourcc, expected_planes, frame.plane_count);
    return false;
  }
  if (frame.plane_fd[0] < 0) {
    ihs::log::warn("[ihs_pv] dma-buf import: fourcc {:#x} has no dma-buf fd",
                   frame.format.fourcc);
    return false;
  }
  // All planes must live in the one dma-buf -- the non-disjoint layout the
  // VAAPI and V4L2 decoders emit, imported through plane_fd[0] with the offsets
  // indexing into it. The IhsFrame contract lets "one fd back several planes",
  // so a secondary plane_fd is either unset (-1) or a duplicate handle to
  // plane_fd[0]; both are accepted. A distinct fd is the disjoint layout this
  // path does not import (it would read the wrong memory).
  for (uint32_t p = 1; p < frame.plane_count; ++p) {
    if (frame.plane_fd[p] >= 0 && frame.plane_fd[p] != frame.plane_fd[0]) {
      ihs::log::warn(
          "[ihs_pv] dma-buf import: multi-planar frame with separate per-plane "
          "fds not supported (planes must share one dma-buf)");
      return false;
    }
  }

  // One explicit layout per plane, all within the single dma-buf's allocation:
  // the driver reads Y and interleaved CbCr from their offsets in the same
  // memory (a non-disjoint multi-planar image).
  std::array<VkSubresourceLayout, 2> plane_layouts{};
  for (uint32_t p = 0; p < frame.plane_count && p < 2; ++p) {
    plane_layouts[p].offset = frame.plane_offset[p];
    plane_layouts[p].rowPitch = frame.plane_stride[p];
  }
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{};
  mod.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  mod.drmFormatModifier = frame.format.modifier;
  mod.drmFormatModifierPlaneCount = frame.plane_count;
  mod.pPlaneLayouts = plane_layouts.data();

  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ext.pNext = &mod;

  VkImageCreateInfo ic{};
  ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ic.pNext = &ext;
  ic.imageType = VK_IMAGE_TYPE_2D;
  ic.format = format;
  ic.extent = {frame.width, frame.height, 1};
  ic.mipLevels = 1;
  ic.arrayLayers = 1;
  ic.samples = VK_SAMPLE_COUNT_1_BIT;
  ic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  // SAMPLED is requested unconditionally, and on at least one driver the
  // modifier does not advertise it (#597). Measured on a Pi 5, V3D 7.1.7.0,
  // VK_FORMAT_B8G8R8A8_UNORM:
  //
  //   modifier                   tilingFeatures  SAMPLED  importable
  //   DRM_FORMAT_MOD_LINEAR      0xc980          no       no
  //   DRM_FORMAT_MOD_BROADCOM_UIF 0xdd83         yes      yes
  //
  // "importable" is vkGetPhysicalDeviceImageFormatProperties2 asked with
  // VkPhysicalDeviceExternalImageFormatInfo, the modifier, and this exact
  // usage: for LINEAR it returns VK_ERROR_FORMAT_NOT_SUPPORTED. Creating the
  // image anyway is invalid, and V3D tolerates it -- vkCreateImage returns
  // VK_SUCCESS and the frames sample and composite correctly.
  //
  // Fixable by offering only the modifiers that pass. What made that look
  // impossible was probing the allocator with SCANOUT, which UIF is refused
  // for; the import path never asks for it, since the frame is sampled through
  // Vulkan rather than handed to a plane. Same board, renderD128, 1280x1440
  // XR24, gbm_bo_create_with_modifiers2:
  //
  //   modifier  usage                result
  //   UIF       RENDERING            OK
  //   UIF       none                 OK
  //   UIF       RENDERING|SCANOUT    failed
  //   LINEAR    RENDERING            OK
  //
  // Run end to end with the capability query filtered to modifiers that pass
  // the importability probe: the board offered UIF for all four packed RGB
  // fourccs, the producer allocated it, and 20 s at 60 submits/s imported with
  // no failures.
  //
  // Two costs go with it, neither fatal and both worth knowing. UIF comes back
  // at the linear stride, so a producer still over-allocates exactly as #598
  // describes. And a CPU producer pays the tiling on every gbm_bo_map/unmap,
  // which is cheap for a small tile and not free at full screen.
  //
  // Still deferred, now on cost rather than impossibility: the defect is
  // invisible in practice, and #582 means this path cannot currently run on
  // the board that exhibits it. The other legal route -- drop SAMPLED when the
  // modifier lacks it and have the compositor vkCmdCopyImage into an
  // optimal-tiled image first -- costs a full-resolution copy per frame and a
  // new ICompositorSurface seam, so it is the fallback, not the plan. #597
  // carries the numbers.
  ic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  // See Init. The driver picks a LINEAR image's layout itself, so it is used
  // only when that layout is the frame's; checked once the image exists.
  const bool linear_tiling = linear_tiling_for_linear_ && !is_yuv &&
                             frame.format.modifier == 0 &&
                             frame.plane_offset[0] == 0;
  if (linear_tiling) {
    ext.pNext = nullptr;
    ic.tiling = VK_IMAGE_TILING_LINEAR;
  }

  VkImage image = VK_NULL_HANDLE;
  const VkResult image_rc = create_image_(device_, &ic, nullptr, &image);
  if (image_rc != VK_SUCCESS) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import: vkCreateImage failed ({}) for {}x{} fourcc "
        "{:#x} modifier {:#x}",
        static_cast<int>(image_rc), frame.width, frame.height,
        frame.format.fourcc, frame.format.modifier);
    return false;
  }
  if (linear_tiling) {
    VkImageSubresource sub{};
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    get_image_subresource_layout_(device_, image, &sub, &layout);
    if (layout.offset != 0 || layout.rowPitch != frame.plane_stride[0]) {
      ihs::log::warn(
          "[ihs_pv] dma-buf import: a {}x{} LINEAR image here has row pitch "
          "{} at offset {}; the frame has stride {}",
          frame.width, frame.height, layout.rowPitch, layout.offset,
          frame.plane_stride[0]);
      destroy_image_(device_, image, nullptr);
      return false;
    }
  }

  VkMemoryRequirements req{};
  get_image_memory_requirements_(device_, image, &req);

  VkMemoryFdPropertiesKHR fd_props{};
  fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
  if (get_memory_fd_properties_(device_,
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                frame.plane_fd[0], &fd_props) != VK_SUCCESS) {
    ihs::log::warn(
        "[ihs_pv] dma-buf import: vkGetMemoryFdPropertiesKHR failed");
    destroy_image_(device_, image, nullptr);
    return false;
  }

  VkPhysicalDeviceMemoryProperties mem_props{};
  get_memory_properties_(physical_device_, &mem_props);
  const uint32_t mt =
      PickMemoryType(mem_props, req.memoryTypeBits & fd_props.memoryTypeBits);
  if (mt == UINT32_MAX) {
    ihs::log::warn("[ihs_pv] dma-buf import: no compatible memory type");
    destroy_image_(device_, image, nullptr);
    return false;
  }

  // Dedicated allocation is required for a dma-buf-backed image on many
  // drivers. It comes first in the chain: VeriSilicon sees it only there, and
  // otherwise imports without error into memory that is not the dma-buf's.
  VkImportMemoryFdInfoKHR import_fd{};
  import_fd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import_fd.fd = frame.plane_fd[0];
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = &import_fd;
  dedicated.image = image;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &dedicated;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  // On success Vulkan owns the imported fd; on failure ownership stays with the
  // caller, so leave it untouched here.
  VkResult alloc_rc = allocate_memory_(device_, &mai, nullptr, &memory);
  if (alloc_rc == VK_ERROR_INVALID_EXTERNAL_HANDLE && import_padding_ != 0) {
    const off_t held = ::lseek(frame.plane_fd[0], 0, SEEK_END);
    ::lseek(frame.plane_fd[0], 0, SEEK_SET);
    const VkDeviceSize ask =
        held < 0 ? 0
                 : ShortImportSize(req.size, static_cast<VkDeviceSize>(held),
                                   import_padding_);
    if (ask != 0) {
      // Out of spec, deliberately. VUID-vkBindImageMemory-size-01049 and
      // VUID-VkMemoryDedicatedAllocateInfo-image-02964 both want
      // allocationSize == req.size, and at that size v3dv requires a dma-buf a
      // page larger than the one v3d's own GBM allocator hands a client for
      // the same image, so such a frame cannot be imported in spec at all
      // (#723, mesa/mesa#16524). The bo v3dv ends up with is still
      // align(req.size, 4096): the short ask drops only the read-ahead slack
      // the GL driver never had either, and the image stays fully backed.
      // Validation flags both VUIDs on every frame that takes this path.
      static std::once_flag warned;
      std::call_once(warned, [padding = import_padding_] {
        ihs::log::warn(
            "[ihs_pv] dma-buf import: v3dv refuses a dma-buf sized to its own "
            "image, so importing it {} bytes short of the requirement -- out "
            "of spec, and reported by validation as "
            "VUID-vkBindImageMemory-size-01049 and "
            "VUID-VkMemoryDedicatedAllocateInfo-image-02964 on every such "
            "frame (#723)",
            padding);
      });
      mai.allocationSize = ask;
      alloc_rc = allocate_memory_(device_, &mai, nullptr, &memory);
    }
  }
  if (alloc_rc != VK_SUCCESS) {
    // The three numbers that explain this failure, and the reason it was
    // unreadable without them: the image's own requirement, what the driver
    // demands of the dma-buf for it (more, where the driver pads an import),
    // and what the dma-buf holds. A short dma-buf means the producer
    // under-allocated; one that covers the image but not the driver's demand
    // is #723.
    const off_t dmabuf_size = ::lseek(frame.plane_fd[0], 0, SEEK_END);
    ihs::log::warn(
        "[ihs_pv] dma-buf import: vkAllocateMemory (import) failed ({}); image "
        "wants {} bytes, the driver requires up to {}, dma-buf holds {} ({}x{} "
        "stride {} modifier {:#x})",
        static_cast<int>(alloc_rc), req.size,
        ImportFootprint(req.size, import_padding_),
        static_cast<long long>(dmabuf_size), frame.width, frame.height,
        frame.plane_stride[0], frame.format.modifier);
    destroy_image_(device_, image, nullptr);
    return false;
  }
  const VkResult bind_rc = bind_image_memory_(device_, image, memory, 0);
  if (bind_rc != VK_SUCCESS) {
    ihs::log::warn("[ihs_pv] dma-buf import: vkBindImageMemory failed ({})",
                   static_cast<int>(bind_rc));
    free_memory_(device_, memory, nullptr);  // closes the imported fd
    destroy_image_(device_, image, nullptr);
    return false;
  }

  out->image = image;
  out->memory = memory;
  out->width = frame.width;
  out->height = frame.height;
  out->format = format;
  out->yuv = is_yuv;
  if (is_yuv) {
    ResolveYcbcr(frame, &out->ycbcr_model, &out->ycbcr_range);
  }
  return true;
}

void DmabufVulkanImporter::Destroy(ImportedImage* image) const {
  if (image == nullptr || !ready()) {
    return;
  }
  if (image->image != VK_NULL_HANDLE) {
    destroy_image_(device_, image->image, nullptr);
    image->image = VK_NULL_HANDLE;
  }
  if (image->memory != VK_NULL_HANDLE) {
    free_memory_(device_, image->memory, nullptr);  // closes the imported fd
    image->memory = VK_NULL_HANDLE;
  }
}
