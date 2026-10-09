// Copyright 2026 Toyota Connected North America
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <flutter_texture_registrar.h>
#include <shell/platform/embedder/embedder.h>

// One registered |kFlutterDesktopGpuSurfaceTypeVkImage| texture.
//
// The registrar map owns it until unregistration; in-flight resolves and
// every frame the engine still holds keep it alive through shared_ptr
// copies, so it outlives the map entry for as long as the engine can call
// back into it.
struct VkImageTexture {
  // Plugin GPU-surface callback, invoked on the raster thread per resolve.
  FlutterDesktopGpuSurfaceTextureCallback callback = nullptr;
  void* user_data = nullptr;

  // Validation limits captured from the backend's device at registration.
  bool allow_ycbcr = false;          // samplerYcbcrConversion enabled
  uint32_t max_image_dimension = 0;  // VkPhysicalDeviceLimits; 0 = unknown

  std::mutex mu;
  std::condition_variable idle_cv;
  // Resolves currently inside |callback| and the thread running them.
  int in_flight = 0;
  std::thread::id resolving_thread;
  // Frames handed to the engine whose destruction callback has not fired.
  size_t outstanding = 0;
  // Set once by RetireVkImageTexture; no resolve starts after it.
  bool retired = false;
  // The plugin's unregister completion, fired once nothing is in flight or
  // outstanding.
  void (*on_retired)(void*) = nullptr;
  void* on_retired_user_data = nullptr;
};

// True if the engine can sample |vk_format| on both its Skia and Impeller
// Vulkan paths. Multi-planar YCbCr formats additionally need |allow_ycbcr|.
bool IsSupportedVkTextureFormat(uint32_t vk_format, bool allow_ycbcr);

// Reads maxImageDimension2D for |physical_device| through
// |get_instance_proc_addr| (a PFN_vkGetInstanceProcAddr). Returns 0 when it
// cannot be queried.
uint32_t QueryMaxImageDimension2D(void* get_instance_proc_addr,
                                  void* instance,
                                  void* physical_device);

// Engine resolve: calls the plugin outside any registrar lock, validates what
// it returns, and fills |texture_out|. Each frame handed to the engine gets an
// embedder-owned release token, so the plugin's release_callback fires at
// most once even if the engine reports a frame twice.
bool ResolveVkImageTexture(const std::shared_ptr<VkImageTexture>& texture,
                           size_t width,
                           size_t height,
                           FlutterVulkanExternalTexture* texture_out);

// Unregistration, after the entry has left the registrar map. No resolve of
// |texture| starts after this call. |callback| fires once the engine has
// released every frame of the texture and no plugin callback is running:
// now, on the calling thread, or later, on an engine thread.
//
// Without a |callback| there is no later signal, so this blocks until a
// resolve running on another thread has returned (one unregistering its own
// texture from inside the plugin callback is not waited on). Returns false in
// that case if the engine still holds frames, whose release callbacks will
// then run after the plugin was told the texture is gone.
bool RetireVkImageTexture(const std::shared_ptr<VkImageTexture>& texture,
                          void (*callback)(void*),
                          void* user_data);

// Frames the engine has not yet released, across all VkImage textures.
// For tests.
size_t OutstandingVkImageFramesForTesting();
