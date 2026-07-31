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

// The same-device import half of the ihs-vk-export path: it imports the
// headless-vulkan backend's exported image pool (dma-buf or opaque-fd memory
// plus the render_done/consumer_done binary semaphores) into its own VkDevice,
// then per presented frame waits render_done, copies the image out to a host
// buffer and checksums it (proving the shared memory carries the rendered
// pixels), and signals consumer_done. It is import-symmetric to the backend's
// export code in headless_vulkan.cc.
//
// This header exposes only C Vulkan handles so the vulkan.hpp dynamic
// dispatcher stays confined to the implementation translation unit.

#ifndef SHELL_BACKEND_HEADLESS_VULKAN_EXPORT_CONSUMER_VK_CONSUMER_H_
#define SHELL_BACKEND_HEADLESS_VULKAN_EXPORT_CONSUMER_VK_CONSUMER_H_

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "wire_protocol.h"

namespace ihs_vke_consumer {

// What ConsumePresent reports about a single frame: a checksum over the whole
// copied-out image plus two sampled pixels, enough to confirm content and to
// see it change frame to frame.
struct FrameStats {
  uint32_t slot = 0;
  uint32_t frame_seq = 0;
  uint64_t crc = 0;        // FNV-1a over width*height*4 bytes
  uint32_t px_first = 0;   // pixel (0,0), packed as read from memory
  uint32_t px_center = 0;  // pixel (width/2, height/2)
};

class VkConsumer {
 public:
  VkConsumer() = default;
  ~VkConsumer();

  VkConsumer(const VkConsumer&) = delete;
  VkConsumer& operator=(const VkConsumer&) = delete;

  // Bring up the loader, a VK_API_VERSION_1_1 instance and a single-GPU device
  // (physical device 0) with the external-memory / external-semaphore / dma-buf
  // extensions enabled, and read the device's VkPhysicalDeviceIDProperties
  // deviceUUID. Returns false (with a logged reason) on any failure.
  bool InitVulkan();

  // The selected device's UUID (16 bytes), for the Hello handshake and the
  // caps deviceUUID comparison. Valid after InitVulkan.
  const uint8_t* device_uuid() const { return device_uuid_; }

  // Import an image table. `descs` holds slot_count descriptors; `fds` holds
  // 3*slot_count descriptors in slot order [mem_0, render_done_0,
  // consumer_done_0, mem_1, ...]. On success the consumer takes ownership of
  // every fd (each is imported into Vulkan, which owns it thereafter) and any
  // previous table is torn down first. On failure every not-yet-consumed fd in
  // `fds` is closed. Returns false on any Vulkan error.
  bool ImportImageTable(uint32_t generation,
                        uint32_t width,
                        uint32_t height,
                        uint32_t handle_type,  // ihs_vke::HandleTypeBits bit
                        const std::vector<ihs_vke::ImageDesc>& descs,
                        std::vector<int>& fds);

  uint32_t generation() const { return generation_; }
  uint32_t slot_count() const { return slot_count_; }

  // Process one presented frame in `slot`: submit a copy-out that waits
  // render_done[slot] exactly once (binary), acquires the image from the
  // foreign producer queue, copies the whole image into the readback buffer,
  // releases it back, and signals consumer_done[slot] exactly once; then
  // CPU-wait the per-slot fence and checksum the pixels into `out`. Returns
  // false on any Vulkan error. The caller sends the FrameRelease pacing edge
  // after this returns.
  bool ConsumePresent(uint32_t slot, uint32_t frame_seq, FrameStats* out);

  // Destroy every Vulkan object and close every owned fd. Idempotent.
  void Teardown();

 private:
  bool CreateInstance();
  bool SelectPhysicalDevice();  // physical device 0
  bool CreateLogicalDevice();
  bool CreateCommandPool();
  bool CreateReadbackBuffer(uint32_t width, uint32_t height);
  void DestroySlots();
  uint32_t FindHostVisibleMemoryType(uint32_t type_bits, bool* coherent) const;

  bool ImportSlotImage(const ihs_vke::ImageDesc& desc,
                       uint32_t slot,
                       int mem_fd);
  bool ImportSlotSemaphore(int fd, VkSemaphore* out);

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = UINT32_MAX;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint8_t device_uuid_[VK_UUID_SIZE] = {};

  std::vector<const char*> enabled_instance_extensions_;
  std::vector<const char*> enabled_device_extensions_;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;

  // One host-visible readback buffer, reused across slots (each present is
  // fenced to completion before the next, so a single buffer never overlaps).
  VkBuffer readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
  void* readback_mapped_ = nullptr;
  VkDeviceSize readback_size_ = 0;
  bool readback_coherent_ = false;

  // Per-slot imported resources (sized to slot_count_).
  std::vector<VkImage> images_;
  std::vector<VkDeviceMemory> image_memory_;
  std::vector<VkSemaphore> render_done_;
  std::vector<VkSemaphore> consumer_done_;
  std::vector<VkCommandBuffer> cmd_;
  std::vector<VkFence> fence_;

  uint32_t generation_ = 0;
  uint32_t slot_count_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t handle_type_ = 0;  // ihs_vke::HandleTypeBits bit for the pool
};

}  // namespace ihs_vke_consumer

#endif  // SHELL_BACKEND_HEADLESS_VULKAN_EXPORT_CONSUMER_VK_CONSUMER_H_
