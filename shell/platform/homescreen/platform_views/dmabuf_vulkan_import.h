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

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "ihs/platform_view.h"

// Imports a plugin-produced dma-buf (IhsFrame) into a VkImage on the
// compositor's own Vulkan device, so an ihs_pv platform view's output is a
// first-class Vulkan resource the compositor blits (and, later, hands to a
// wl_subsurface / KMS plane for a copy-free path). The imported image aliases
// the dma-buf's memory — no pixel copy; only the VkImage/VkDeviceMemory
// wrappers are created, once per ring buffer.
//
// All Vulkan entry points are resolved through the backend's interposed
// get_instance_proc_addr, so submissions are serialized on the shared queue the
// same way the engine's are. The importer owns nothing beyond the resolved
// function pointers; the caller owns the ImportedImage lifetimes.
class DmabufVulkanImporter {
 public:
  struct ImportedImage {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
  };

  DmabufVulkanImporter() = default;

  // Resolve the device entry points from @get_instance_proc_addr. Returns false
  // when the device lacks the dma-buf import extensions (the plugin then falls
  // back to the software floor). Call once before Import.
  bool Init(VkInstance instance,
            VkPhysicalDevice physical_device,
            VkDevice device,
            void* get_instance_proc_addr);

  [[nodiscard]] bool ready() const { return device_ != VK_NULL_HANDLE; }

  // Import @frame's first-plane dma-buf into @out. On success the imported
  // image has taken ownership of frame.plane_fd[0] (freed with the memory in
  // Destroy); on failure the fd is left untouched for the caller to close.
  // Single-plane RGB only for now (the YUV multi-plane path lands with video).
  bool Import(const IhsFrame& frame, ImportedImage* out) const;

  void Destroy(ImportedImage* image) const;

 private:
  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};

  PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties_{nullptr};
  PFN_vkCreateImage create_image_{nullptr};
  PFN_vkDestroyImage destroy_image_{nullptr};
  PFN_vkGetImageMemoryRequirements get_image_memory_requirements_{nullptr};
  PFN_vkAllocateMemory allocate_memory_{nullptr};
  PFN_vkFreeMemory free_memory_{nullptr};
  PFN_vkBindImageMemory bind_image_memory_{nullptr};
  PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_{nullptr};
};
