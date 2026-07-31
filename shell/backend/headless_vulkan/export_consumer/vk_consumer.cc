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

// Use vulkan.hpp's dynamic dispatch loader, matching the shell's Vulkan
// backends. The loader dlopens libvulkan at runtime, so this binary needs no
// Vulkan link library -- only the vendored headers at build time. This is the
// one translation unit that includes vulkan.hpp, so it owns the dispatcher's
// single storage definition.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "vk_consumer.h"

#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>

namespace ihs_vke_consumer {

namespace {

// Accessor for the dynamic dispatcher, matching headless_vulkan.cc's idiom.
const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

void LogErr(const char* fmt, ...) {
  std::fputs("[ihs-vk-consumer] ERROR: ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

void LogInfo(const char* fmt, ...) {
  std::fputs("[ihs-vk-consumer] ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

bool HasExt(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

// The usage the exported pool images are created with (see
// headless_vulkan.cc's kRenderTargetUsage). Recreating the imported image with
// the same usage keeps its memory requirements compatible with the exporter's,
// which the opaque-fd import in particular relies on, and keeps a chosen DRM
// modifier valid for the dma-buf import.
constexpr VkImageUsageFlags kImportUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

}  // namespace

VkConsumer::~VkConsumer() {
  Teardown();
}

bool VkConsumer::InitVulkan() {
  try {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();
  } catch (const std::exception& e) {
    LogErr("Vulkan loader not present: %s", e.what());
    return false;
  }
  if (d().vkCreateInstance == nullptr) {
    LogErr("vkCreateInstance unresolved");
    return false;
  }
  return CreateInstance() && SelectPhysicalDevice() && CreateLogicalDevice() &&
         CreateCommandPool();
}

bool VkConsumer::CreateInstance() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "ihs-vk-export-consumer";
  app.apiVersion = VK_API_VERSION_1_1;

  // These are core in 1.1; enable them only when the loader still advertises
  // them as instance extensions so vkCreateInstance never fails on a driver
  // that folded them into core.
  uint32_t ext_count = 0;
  d().vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail(ext_count);
  if (ext_count > 0) {
    d().vkEnumerateInstanceExtensionProperties(nullptr, &ext_count,
                                               avail.data());
  }
  for (const char* opt :
       {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME}) {
    if (HasExt(avail, opt)) {
      enabled_instance_extensions_.push_back(opt);
    }
  }

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_instance_extensions_.size());
  info.ppEnabledExtensionNames = enabled_instance_extensions_.data();

  if (d().vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
    LogErr("vkCreateInstance failed");
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
  return true;
}

bool VkConsumer::SelectPhysicalDevice() {
  uint32_t count = 0;
  d().vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  if (count == 0) {
    LogErr("no Vulkan physical devices");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  d().vkEnumeratePhysicalDevices(instance_, &count, devices.data());

  // Single-GPU target (Raspberry Pi V3D): take physical device 0.
  physical_device_ = devices[0];

  VkPhysicalDeviceProperties props{};
  d().vkGetPhysicalDeviceProperties(physical_device_, &props);
  if (props.apiVersion < VK_API_VERSION_1_1) {
    LogErr("physical device 0 (%s) exposes Vulkan < 1.1", props.deviceName);
    return false;
  }

  VkPhysicalDeviceIDProperties id{};
  id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  p2.pNext = &id;
  d().vkGetPhysicalDeviceProperties2(physical_device_, &p2);
  std::memcpy(device_uuid_, id.deviceUUID, VK_UUID_SIZE);

  uint32_t qf_count = 0;
  d().vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count,
                                               nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  if (qf_count > 0) {
    d().vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count,
                                                 qfs.data());
  }
  for (uint32_t i = 0; i < qfs.size(); ++i) {
    // A graphics queue also supports transfer + the queue-family-ownership
    // barriers this consumer issues.
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_ = i;
      break;
    }
  }
  if (queue_family_ == UINT32_MAX) {
    LogErr("physical device 0 (%s) has no graphics queue", props.deviceName);
    return false;
  }

  LogInfo("selected physical device 0: '%s'", props.deviceName);
  return true;
}

bool VkConsumer::CreateLogicalDevice() {
  uint32_t ext_count = 0;
  d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                           &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail(ext_count);
  if (ext_count > 0) {
    d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                             &ext_count, avail.data());
  }

  // The import path needs all of these; a missing one is a hard failure so the
  // reason is clear rather than surfacing later as a failed import.
  static constexpr std::array<const char*, 7> kRequired = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
  };
  for (const char* req : kRequired) {
    if (!HasExt(avail, req)) {
      LogErr("device is missing required extension %s", req);
      return false;
    }
    enabled_device_extensions_.push_back(req);
  }

  constexpr float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();

  if (d().vkCreateDevice(physical_device_, &device_info, nullptr, &device_) !=
      VK_SUCCESS) {
    LogErr("vkCreateDevice failed");
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));
  d().vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  return true;
}

bool VkConsumer::CreateCommandPool() {
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = queue_family_;
  if (d().vkCreateCommandPool(device_, &cpi, nullptr, &command_pool_) !=
      VK_SUCCESS) {
    LogErr("vkCreateCommandPool failed");
    return false;
  }
  return true;
}

uint32_t VkConsumer::FindHostVisibleMemoryType(uint32_t type_bits,
                                               bool* coherent) const {
  VkPhysicalDeviceMemoryProperties mem{};
  d().vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem);
  // Prefer a coherent host-visible type (no explicit invalidate needed), but
  // accept a non-coherent one and invalidate before reading.
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) == 0) {
      continue;
    }
    const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
    if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
      continue;
    }
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      *coherent = true;
      return i;
    }
    if (fallback == UINT32_MAX) {
      fallback = i;
    }
  }
  *coherent = false;
  return fallback;
}

bool VkConsumer::CreateReadbackBuffer(uint32_t width, uint32_t height) {
  const VkDeviceSize needed = static_cast<VkDeviceSize>(width) * height * 4;
  if (readback_buffer_ != VK_NULL_HANDLE && readback_size_ >= needed) {
    return true;  // an existing buffer is large enough
  }
  // Drop any prior (too-small) buffer.
  if (readback_memory_ != VK_NULL_HANDLE) {
    d().vkUnmapMemory(device_, readback_memory_);
    d().vkFreeMemory(device_, readback_memory_, nullptr);
    readback_memory_ = VK_NULL_HANDLE;
    readback_mapped_ = nullptr;
  }
  if (readback_buffer_ != VK_NULL_HANDLE) {
    d().vkDestroyBuffer(device_, readback_buffer_, nullptr);
    readback_buffer_ = VK_NULL_HANDLE;
  }
  readback_size_ = 0;

  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = needed;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (d().vkCreateBuffer(device_, &bci, nullptr, &readback_buffer_) !=
      VK_SUCCESS) {
    LogErr("vkCreateBuffer (readback) failed");
    return false;
  }

  VkMemoryRequirements req{};
  d().vkGetBufferMemoryRequirements(device_, readback_buffer_, &req);
  const uint32_t mt =
      FindHostVisibleMemoryType(req.memoryTypeBits, &readback_coherent_);
  if (mt == UINT32_MAX) {
    LogErr("no host-visible memory type for readback buffer");
    return false;
  }

  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;
  if (d().vkAllocateMemory(device_, &mai, nullptr, &readback_memory_) !=
      VK_SUCCESS) {
    LogErr("vkAllocateMemory (readback) failed");
    return false;
  }
  if (d().vkBindBufferMemory(device_, readback_buffer_, readback_memory_, 0) !=
      VK_SUCCESS) {
    LogErr("vkBindBufferMemory (readback) failed");
    return false;
  }
  if (d().vkMapMemory(device_, readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      &readback_mapped_) != VK_SUCCESS) {
    LogErr("vkMapMemory (readback) failed");
    return false;
  }
  readback_size_ = req.size;
  return true;
}

bool VkConsumer::ImportSlotImage(const ihs_vke::ImageDesc& desc,
                                 uint32_t slot,
                                 int mem_fd) {
  const bool dmabuf = handle_type_ == ihs_vke::kHandleDmaBuf;
  const VkExternalMemoryHandleTypeFlagBits handle =
      dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
             : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  const auto format = static_cast<VkFormat>(desc.vk_format);

  // Explicit per-plane layout for the dma-buf import: the exporter published
  // the modifier and each plane's offset/pitch; reconstruct the same tiling so
  // the imported image aliases the exact bytes the backend rendered.
  std::array<VkSubresourceLayout, ihs_vke::kMaxPlanes> plane_layouts{};
  const uint32_t plane_count = dmabuf ? desc.plane_count : 1u;
  for (uint32_t p = 0; p < plane_count && p < ihs_vke::kMaxPlanes; ++p) {
    plane_layouts[p].offset = desc.plane_offset[p];
    plane_layouts[p].rowPitch = desc.plane_pitch[p];
  }
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{};
  mod.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  mod.drmFormatModifier = desc.drm_modifier;
  mod.drmFormatModifierPlaneCount = plane_count;
  mod.pPlaneLayouts = plane_layouts.data();

  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = handle;
  if (dmabuf) {
    ext.pNext = &mod;
  }

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.pNext = &ext;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {desc.width, desc.height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = dmabuf ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                      : VK_IMAGE_TILING_OPTIMAL;
  ici.usage = kImportUsage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (d().vkCreateImage(device_, &ici, nullptr, &images_[slot]) != VK_SUCCESS) {
    LogErr("slot %u: vkCreateImage failed", slot);
    return false;
  }

  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device_, images_[slot], &req);

  // Memory type: for a dma-buf, intersect the image's requirement with the
  // fd's compatible types; for an opaque fd, reuse the exporter's published
  // memory_type_index (opaque memory must import onto the same type).
  uint32_t mt = UINT32_MAX;
  if (dmabuf) {
    VkMemoryFdPropertiesKHR fdp{};
    fdp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (d().vkGetMemoryFdPropertiesKHR(device_, handle, mem_fd, &fdp) !=
        VK_SUCCESS) {
      LogErr("slot %u: vkGetMemoryFdPropertiesKHR failed", slot);
      return false;
    }
    const uint32_t allowed = req.memoryTypeBits & fdp.memoryTypeBits;
    for (uint32_t i = 0; i < 32; ++i) {
      if (allowed & (1u << i)) {
        mt = i;
        break;
      }
    }
  } else {
    mt = desc.memory_type_index;
  }
  if (mt == UINT32_MAX) {
    LogErr("slot %u: no compatible memory type for import", slot);
    return false;
  }

  // A dma-buf-backed image requires a dedicated allocation on most drivers;
  // the exporter also allocated the backing memory dedicated per image.
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = images_[slot];
  VkImportMemoryFdInfoKHR import_fd{};
  import_fd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import_fd.pNext = &dedicated;
  import_fd.handleType = handle;
  import_fd.fd = mem_fd;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &import_fd;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mt;

  // On success Vulkan owns the imported fd; on failure it stays with the caller
  // (ImportImageTable closes it).
  if (d().vkAllocateMemory(device_, &mai, nullptr, &image_memory_[slot]) !=
      VK_SUCCESS) {
    LogErr("slot %u: vkAllocateMemory (import) failed", slot);
    return false;
  }
  if (d().vkBindImageMemory(device_, images_[slot], image_memory_[slot], 0) !=
      VK_SUCCESS) {
    LogErr("slot %u: vkBindImageMemory failed", slot);
    return false;
  }
  return true;
}

bool VkConsumer::ImportSlotSemaphore(int fd, VkSemaphore* out) {
  VkSemaphoreCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;  // binary
  if (d().vkCreateSemaphore(device_, &sci, nullptr, out) != VK_SUCCESS) {
    LogErr("vkCreateSemaphore (import target) failed");
    return false;
  }
  // Permanent OPAQUE_FD import: the imported semaphore shares the exporter's
  // payload, so a signal on the backend side satisfies a wait here (and vice
  // versa) across every frame. Vulkan owns the fd on success.
  VkImportSemaphoreFdInfoKHR ifi{};
  ifi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
  ifi.semaphore = *out;
  ifi.flags = 0;  // permanent
  ifi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  ifi.fd = fd;
  if (d().vkImportSemaphoreFdKHR(device_, &ifi) != VK_SUCCESS) {
    LogErr("vkImportSemaphoreFdKHR (OPAQUE_FD) failed");
    return false;
  }
  return true;
}

bool VkConsumer::ImportImageTable(uint32_t generation,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t handle_type,
                                  const std::vector<ihs_vke::ImageDesc>& descs,
                                  std::vector<int>& fds) {
  const uint32_t slots = static_cast<uint32_t>(descs.size());
  if (slots == 0 || slots > ihs_vke::kMaxSlots ||
      fds.size() != ihs_vke::ImageTableFdCount(slots)) {
    LogErr("image table shape invalid (slots=%u fds=%zu)", slots, fds.size());
    return false;
  }
  if (handle_type != ihs_vke::kHandleDmaBuf &&
      handle_type != ihs_vke::kHandleOpaqueFd) {
    LogErr("unsupported handle type 0x%x", handle_type);
    return false;
  }

  // Idle and drop any previous table before importing the new one.
  if (device_ != VK_NULL_HANDLE) {
    d().vkDeviceWaitIdle(device_);
  }
  DestroySlots();

  handle_type_ = handle_type;
  width_ = width;
  height_ = height;
  generation_ = generation;
  slot_count_ = slots;

  images_.assign(slots, VK_NULL_HANDLE);
  image_memory_.assign(slots, VK_NULL_HANDLE);
  render_done_.assign(slots, VK_NULL_HANDLE);
  consumer_done_.assign(slots, VK_NULL_HANDLE);
  cmd_.assign(slots, VK_NULL_HANDLE);
  fence_.assign(slots, VK_NULL_HANDLE);

  // Ownership discipline: mark each fd -1 as it is consumed (imported); any fd
  // still set at the end of a failed import is closed by the cleanup below.
  auto take = [&](size_t idx) -> int {
    const int fd = fds[idx];
    fds[idx] = -1;
    return fd;
  };
  auto fail = [&]() -> bool {
    for (int& fd : fds) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
    if (device_ != VK_NULL_HANDLE) {
      d().vkDeviceWaitIdle(device_);
    }
    DestroySlots();
    slot_count_ = 0;
    return false;
  };

  if (!CreateReadbackBuffer(width, height)) {
    return fail();
  }

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = command_pool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;

  for (uint32_t s = 0; s < slots; ++s) {
    const int mem_fd = take(3u * s + 0);
    const int render_done_fd = take(3u * s + 1);
    const int consumer_done_fd = take(3u * s + 2);
    if (mem_fd < 0 || render_done_fd < 0 || consumer_done_fd < 0) {
      LogErr("slot %u: missing fd(s) in image table", s);
      // Close whichever of this slot's fds we did receive.
      if (mem_fd >= 0) {
        ::close(mem_fd);
      }
      if (render_done_fd >= 0) {
        ::close(render_done_fd);
      }
      if (consumer_done_fd >= 0) {
        ::close(consumer_done_fd);
      }
      return fail();
    }

    if (!ImportSlotImage(descs[s], s, mem_fd)) {
      ::close(mem_fd);  // allocate failed before taking ownership
      ::close(render_done_fd);
      ::close(consumer_done_fd);
      return fail();
    }
    // ImportSlotImage's vkAllocateMemory consumed mem_fd on success.
    if (!ImportSlotSemaphore(render_done_fd, &render_done_[s])) {
      ::close(render_done_fd);
      ::close(consumer_done_fd);
      return fail();
    }
    if (!ImportSlotSemaphore(consumer_done_fd, &consumer_done_[s])) {
      ::close(consumer_done_fd);
      return fail();
    }

    if (d().vkAllocateCommandBuffers(device_, &cbai, &cmd_[s]) != VK_SUCCESS) {
      LogErr("slot %u: vkAllocateCommandBuffers failed", s);
      return fail();
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;  // unsignaled
    if (d().vkCreateFence(device_, &fci, nullptr, &fence_[s]) != VK_SUCCESS) {
      LogErr("slot %u: vkCreateFence failed", s);
      return fail();
    }
  }

  LogInfo("imported image table: generation=%u slots=%u %ux%u handle=%s",
          generation_, slot_count_, width_, height_,
          handle_type_ == ihs_vke::kHandleDmaBuf ? "dmabuf" : "opaque-fd");
  return true;
}

bool VkConsumer::ConsumePresent(uint32_t slot,
                                uint32_t frame_seq,
                                FrameStats* out) {
  if (slot >= slot_count_ || images_[slot] == VK_NULL_HANDLE) {
    LogErr("present for invalid slot %u (slot_count=%u)", slot, slot_count_);
    return false;
  }

  VkCommandBuffer cmd = cmd_[slot];
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (d().vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    LogErr("slot %u: vkBeginCommandBuffer failed", slot);
    return false;
  }

  const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                            1};

  // Acquire the image from the foreign producer queue (the backend rendered it
  // and does not do a matching Vulkan queue-family release, so FOREIGN_EXT
  // models the external producer). oldLayout UNDEFINED is used because we only
  // copy the pixels out: for the dma-buf path the DRM modifier pins the memory
  // layout so the content survives the transition, and render_done gates the
  // read either way. (See the note in the file/report about the opaque-fd
  // path, where OPTIMAL tiling is not modifier-pinned.)
  VkImageMemoryBarrier acquire{};
  acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  acquire.dstQueueFamilyIndex = queue_family_;
  acquire.srcAccessMask = 0;
  acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  acquire.image = images_[slot];
  acquire.subresourceRange = color_range;
  d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &acquire);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;    // tightly packed to width
  region.bufferImageHeight = 0;  // tightly packed to height
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width_, height_, 1};
  d().vkCmdCopyImageToBuffer(cmd, images_[slot],
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback_buffer_, 1, &region);

  // Release the image back to the foreign producer queue so the backend can
  // re-render this slot after it observes consumer_done.
  VkImageMemoryBarrier release{};
  release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  release.srcQueueFamilyIndex = queue_family_;
  release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  release.dstAccessMask = 0;
  release.image = images_[slot];
  release.subresourceRange = color_range;
  d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &release);

  if (d().vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    LogErr("slot %u: vkEndCommandBuffer failed", slot);
    return false;
  }

  // Exactly one wait on render_done[slot] and one signal of consumer_done[slot]
  // per present, keeping the binary-semaphore pairing with the backend strict.
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &render_done_[slot];
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &consumer_done_[slot];
  if (d().vkQueueSubmit(queue_, 1, &si, fence_[slot]) != VK_SUCCESS) {
    LogErr("slot %u: vkQueueSubmit failed", slot);
    return false;
  }
  if (d().vkWaitForFences(device_, 1, &fence_[slot], VK_TRUE, UINT64_MAX) !=
      VK_SUCCESS) {
    LogErr("slot %u: vkWaitForFences failed", slot);
    return false;
  }
  d().vkResetFences(device_, 1, &fence_[slot]);

  if (!readback_coherent_) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readback_memory_;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    d().vkInvalidateMappedMemoryRanges(device_, 1, &range);
  }

  // FNV-1a over the copied pixels: proves the shared memory carries the
  // backend's rendered frame and that it changes frame to frame.
  const auto* bytes = static_cast<const uint8_t*>(readback_mapped_);
  const size_t n = static_cast<size_t>(width_) * height_ * 4;
  uint64_t crc = 1469598103934665603ULL;  // FNV offset basis
  for (size_t i = 0; i < n; ++i) {
    crc ^= bytes[i];
    crc *= 1099511628211ULL;  // FNV prime
  }

  const auto* px = reinterpret_cast<const uint32_t*>(bytes);
  const size_t center =
      (static_cast<size_t>(height_ / 2) * width_) + width_ / 2;

  if (out != nullptr) {
    out->slot = slot;
    out->frame_seq = frame_seq;
    out->crc = crc;
    out->px_first = px[0];
    out->px_center = center < (n / 4) ? px[center] : 0;
  }
  return true;
}

void VkConsumer::DestroySlots() {
  for (size_t i = 0; i < slot_count_; ++i) {
    if (i < fence_.size() && fence_[i] != VK_NULL_HANDLE) {
      d().vkDestroyFence(device_, fence_[i], nullptr);
    }
    if (i < cmd_.size() && cmd_[i] != VK_NULL_HANDLE) {
      d().vkFreeCommandBuffers(device_, command_pool_, 1, &cmd_[i]);
    }
    if (i < render_done_.size() && render_done_[i] != VK_NULL_HANDLE) {
      d().vkDestroySemaphore(device_, render_done_[i], nullptr);
    }
    if (i < consumer_done_.size() && consumer_done_[i] != VK_NULL_HANDLE) {
      d().vkDestroySemaphore(device_, consumer_done_[i], nullptr);
    }
    if (i < images_.size() && images_[i] != VK_NULL_HANDLE) {
      d().vkDestroyImage(device_, images_[i], nullptr);
    }
    if (i < image_memory_.size() && image_memory_[i] != VK_NULL_HANDLE) {
      d().vkFreeMemory(device_, image_memory_[i],
                       nullptr);  // closes imported fd
    }
  }
  images_.clear();
  image_memory_.clear();
  render_done_.clear();
  consumer_done_.clear();
  cmd_.clear();
  fence_.clear();
}

void VkConsumer::Teardown() {
  if (device_ != VK_NULL_HANDLE) {
    d().vkDeviceWaitIdle(device_);
    DestroySlots();
    slot_count_ = 0;
    if (readback_memory_ != VK_NULL_HANDLE) {
      d().vkUnmapMemory(device_, readback_memory_);
      d().vkFreeMemory(device_, readback_memory_, nullptr);
      readback_memory_ = VK_NULL_HANDLE;
      readback_mapped_ = nullptr;
    }
    if (readback_buffer_ != VK_NULL_HANDLE) {
      d().vkDestroyBuffer(device_, readback_buffer_, nullptr);
      readback_buffer_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
      d().vkDestroyCommandPool(device_, command_pool_, nullptr);
      command_pool_ = VK_NULL_HANDLE;
    }
    d().vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    d().vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

}  // namespace ihs_vke_consumer
