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

#ifndef SHELL_BACKEND_WAYLAND_VULKAN_VULKAN_QUEUE_INTERPOSER_H_
#define SHELL_BACKEND_WAYLAND_VULKAN_VULKAN_QUEUE_INTERPOSER_H_

#include <mutex>

#include <vulkan/vulkan.h>

namespace wayland_vulkan {

/**
 * @brief Serializes host access to a VkQueue shared between the Flutter
 *        engine and the backend.
 *
 * The Vulkan spec requires host access to a VkQueue to be externally
 * synchronized. On this backend the Flutter raster thread (engine-internal
 * vkQueueSubmit / vkQueueSubmit2) and the platform thread (present path,
 * one-shot transfer helpers, swapchain-recreation drains) both use the
 * single graphics queue handed to the engine via
 * FlutterVulkanRendererConfig.
 *
 * The embedder API designates get_instance_proc_address_callback as the
 * mechanism for making this safe: the embedder is expected to swap out
 * vkQueue* entry points for locking variants (see the field documentation
 * in flutter/shell/platform/embedder/embedder.h and the
 * EmbedderTest.CanSwapOutVulkanCalls reference test). This class implements
 * that contract:
 *
 *  - RegisterQueue() associates a queue handle with the mutex that already
 *    guards the backend's own submissions (queue_mutex_), so both sides
 *    serialize on the same lock.
 *  - Interpose() recognizes the externally-synchronized vkQueue* entry
 *    points (and vkGetDeviceProcAddr, so device-level resolution is also
 *    covered), caches the real driver proc, and returns a trampoline that
 *    takes the registered mutex for the duration of the call.
 *
 * Both engine resolution paths are covered: Impeller resolves every proc
 * through the instance-level callback (vulkan.hpp dispatcher initialized
 * with the embedder GIPA), while the Skia VulkanProcTable resolves device
 * procs through a vkGetDeviceProcAddr that it obtained from the same
 * callback — which Interpose() shims.
 *
 * Calls on a queue that was never registered (or was unregistered) pass
 * through to the real proc without locking.
 */
class QueueInterposer {
 public:
  QueueInterposer() = delete;

  /**
   * @brief Associate a queue with the mutex guarding all host access to it.
   *
   * Call once per queue, after vkGetDeviceQueue and before the engine is
   * handed the queue. The mutex must outlive every submission on the queue;
   * unregister before it (or the queue's device) is destroyed.
   */
  static void RegisterQueue(VkQueue queue, std::mutex* mutex);

  /**
   * @brief Remove a queue's registration. Safe to call for a queue that was
   *        never registered.
   */
  static void UnregisterQueue(VkQueue queue);

  /**
   * @brief Interpose a proc-address lookup from the engine.
   *
   * If @p procname is a vkQueue* entry point requiring external
   * synchronization (vkQueueSubmit, vkQueueSubmit2[KHR], vkQueueWaitIdle,
   * vkQueuePresentKHR, vkQueueBindSparse, vkQueue*DebugUtilsLabelEXT) or
   * vkGetDeviceProcAddr, resolves the real proc via @p gipa, caches it, and
   * returns a locking trampoline. Returns nullptr for every other name; the
   * caller should then fall through to its normal resolution.
   */
  static PFN_vkVoidFunction Interpose(VkInstance instance,
                                      const char* procname,
                                      PFN_vkGetInstanceProcAddr gipa);
};

}  // namespace wayland_vulkan

#endif  // SHELL_BACKEND_WAYLAND_VULKAN_VULKAN_QUEUE_INTERPOSER_H_
