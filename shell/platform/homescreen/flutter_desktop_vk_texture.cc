// Copyright 2026 Toyota Connected North America
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter_desktop_vk_texture.h"

#include "flutter_desktop_texture_registrar.h"

#include <cstddef>
#include <unordered_map>
#include <utility>

#include <vulkan/vulkan_core.h>

namespace {

// True when the engine-allocated |FlutterVulkanExternalTexture| is large
// enough to hold |member|. The engine sets |struct_size| to its own sizeof
// before invoking the callback, so an engine built against an older header
// hands us a shorter struct.
#define IHS_VK_TEX_HAS(ptr, member) \
  ((ptr)->struct_size >=            \
   offsetof(FlutterVulkanExternalTexture, member) + sizeof((ptr)->member))

// A frame the engine holds. The engine is handed an opaque token rather than
// a pointer to this record, so a second destruction call for the same frame
// finds nothing instead of touching freed memory. (The engine's Skia path
// can report a rejected image twice: SkImages::BorrowTextureFrom releases it
// on failure, and the engine then calls the release proc itself.)
struct FrameRelease {
  std::shared_ptr<VkImageTexture> texture;
  void (*release)(void*) = nullptr;
  void* release_context = nullptr;
};

// Leaked on purpose: an engine thread can still release a frame while the
// process runs its static destructors (exit() with the engine up).
struct FrameTable {
  std::mutex mu;
  std::unordered_map<uintptr_t, FrameRelease> frames;
  uintptr_t next_token = 1;
};

FrameTable& Frames() {
  static auto* table = new FrameTable();
  return *table;
}

// Fires the plugin's unregister completion once the texture is retired and
// the engine holds nothing of it. Runs at most once: the callback is taken
// under the lock.
void MaybeFinishRetire(const std::shared_ptr<VkImageTexture>& texture) {
  void (*callback)(void*) = nullptr;
  void* user_data = nullptr;
  {
    std::scoped_lock lock(texture->mu);
    if (!texture->retired || texture->in_flight != 0 ||
        texture->outstanding != 0) {
      return;
    }
    callback = std::exchange(texture->on_retired, nullptr);
    user_data = std::exchange(texture->on_retired_user_data, nullptr);
  }
  if (callback) {
    callback(user_data);
  }
}

void OnEngineReleasedFrame(void* token) {
  FrameRelease frame;
  {
    auto& table = Frames();
    std::scoped_lock lock(table.mu);
    const auto it = table.frames.find(reinterpret_cast<uintptr_t>(token));
    if (it == table.frames.end()) {
      return;  // already released
    }
    frame = std::move(it->second);
    table.frames.erase(it);
  }
  if (frame.release) {
    frame.release(frame.release_context);
  }
  {
    std::scoped_lock lock(frame.texture->mu);
    --frame.texture->outstanding;
  }
  MaybeFinishRetire(frame.texture);
}

uintptr_t TrackFrame(FrameRelease frame) {
  auto& table = Frames();
  std::scoped_lock lock(table.mu);
  // Skip 0 ("no user data") and, once the counter has wrapped (2^32 frames on
  // a 32-bit target), any token a long-held frame still owns.
  uintptr_t token;
  do {
    token = table.next_token++;
  } while (token == 0 || table.frames.count(token) != 0);
  table.frames.emplace(token, std::move(frame));
  return token;
}

bool IsYcbcrFormat(uint32_t vk_format) {
  // The multi-planar formats the engine routes through a
  // VkSamplerYcbcrConversion (IsYuvFormat in
  // embedder_external_texture_vulkan.cc).
  switch (vk_format) {
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IsSupportedVkTextureFormat(uint32_t vk_format, bool allow_ycbcr) {
  // Color formats both engine paths map: the intersection of ToSkColorType
  // and the Impeller ToPixelFormat in embedder_external_texture_vulkan.cc.
  // Anything else reaches vkCreateImageView (Impeller) with a format the
  // engine cannot describe, or is rejected by Skia.
  switch (vk_format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
      return true;
    default:
      // Creating a VkSamplerYcbcrConversion on a device without the
      // samplerYcbcrConversion feature is undefined behavior, and Impeller
      // creates one unconditionally for these formats.
      return allow_ycbcr && IsYcbcrFormat(vk_format);
  }
}

uint32_t QueryMaxImageDimension2D(void* get_instance_proc_addr,
                                  void* instance,
                                  void* physical_device) {
  if (!get_instance_proc_addr || !instance || !physical_device) {
    return 0;
  }
  const auto gipa =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
  const auto get_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(
          static_cast<VkInstance>(instance), "vkGetPhysicalDeviceProperties"));
  if (!get_properties) {
    return 0;
  }
  VkPhysicalDeviceProperties properties{};
  get_properties(static_cast<VkPhysicalDevice>(physical_device), &properties);
  return properties.limits.maxImageDimension2D;
}

bool ResolveVkImageTexture(const std::shared_ptr<VkImageTexture>& texture,
                           size_t width,
                           size_t height,
                           FlutterVulkanExternalTexture* texture_out) {
  // Every engine with this API has |destruction_callback|; without it a frame
  // could never be released and unregistration would never complete.
  if (!texture || !texture_out ||
      !IHS_VK_TEX_HAS(texture_out, destruction_callback)) {
    return false;
  }

  FlutterDesktopGpuSurfaceTextureCallback callback;
  void* user_data;
  {
    std::scoped_lock lock(texture->mu);
    if (texture->retired || !texture->callback) {
      return false;
    }
    ++texture->in_flight;
    texture->resolving_thread = std::this_thread::get_id();
    callback = texture->callback;
    user_data = texture->user_data;
  }

  // No lock is held here: the contract has the plugin host-sync in this
  // call, and it may register or unregister textures from it.
  const FlutterDesktopGpuSurfaceDescriptor* surface =
      callback(width, height, user_data);

  void (*release)(void*) = nullptr;
  void* release_context = nullptr;
  const FlutterDesktopVulkanImage* image = nullptr;
  if (surface && surface->struct_size == sizeof(*surface)) {
    release = surface->release_callback;
    release_context = surface->release_context;
    image = static_cast<const FlutterDesktopVulkanImage*>(surface->handle);
  }
  const uint32_t max_dim = texture->max_image_dimension;
  const bool usable =
      image && image->struct_size >= sizeof(FlutterDesktopVulkanImage) &&
      image->image != 0 &&
      IsSupportedVkTextureFormat(image->format, texture->allow_ycbcr) &&
      (max_dim == 0 ||
       (surface->width <= max_dim && surface->height <= max_dim));

  bool handed_over = false;
  {
    std::scoped_lock lock(texture->mu);
    --texture->in_flight;
    if (usable && !texture->retired) {
      ++texture->outstanding;
      handed_over = true;
    }
  }
  texture->idle_cv.notify_all();

  if (!handed_over) {
    // No frame, an unusable one, or the texture was unregistered from inside
    // the callback. Give a produced frame straight back so the producer is
    // not left waiting on a release that never comes.
    if (release) {
      release(release_context);
    }
    MaybeFinishRetire(texture);
    return false;
  }

  const uintptr_t token =
      TrackFrame(FrameRelease{texture, release, release_context});
  texture_out->image = static_cast<FlutterVulkanImageHandle>(image->image);
  texture_out->format = image->format;
  texture_out->user_data = reinterpret_cast<void*>(token);
  texture_out->destruction_callback = &OnEngineReleasedFrame;
  if (IHS_VK_TEX_HAS(texture_out, height)) {
    texture_out->width = surface->width;
    texture_out->height = surface->height;
  }
  return true;
}

#undef IHS_VK_TEX_HAS

bool RetireVkImageTexture(const std::shared_ptr<VkImageTexture>& texture,
                          void (*callback)(void*),
                          void* user_data) {
  if (!texture) {
    if (callback) {
      callback(user_data);
    }
    return true;
  }
  bool released;
  {
    std::unique_lock lock(texture->mu);
    texture->retired = true;
    texture->on_retired = callback;
    texture->on_retired_user_data = user_data;
    // With a completion callback, nothing needs to block: no resolve starts
    // after |retired|, one already inside the plugin hands its frame back,
    // and the callback waits for both. Without one, returning is the only
    // signal the plugin gets, so at least keep its GPU-surface callback from
    // running past it -- unless this is that callback unregistering its own
    // texture, which waiting on would deadlock.
    const bool reentrant =
        texture->in_flight > 0 &&
        texture->resolving_thread == std::this_thread::get_id();
    if (!callback && !reentrant) {
      texture->idle_cv.wait(lock, [&] { return texture->in_flight == 0; });
    }
    released = texture->outstanding == 0;
  }
  MaybeFinishRetire(texture);
  return callback != nullptr || released;
}

bool PopulateExternalVulkanTextureFrame(
    FlutterDesktopTextureRegistrar* texture_registrar,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterVulkanExternalTexture* texture_out) {
  if (!texture_registrar ||
      texture_registrar->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  std::shared_ptr<VkImageTexture> texture;
  {
    std::scoped_lock lock(texture_registrar->texture_mutex);
    const auto it = texture_registrar->texture_registry.find(texture_id);
    if (it == texture_registrar->texture_registry.end() || !it->second) {
      return false;
    }
    texture = it->second->vk_image;
  }
  return ResolveVkImageTexture(texture, width, height, texture_out);
}

size_t OutstandingVkImageFramesForTesting() {
  auto& table = Frames();
  std::scoped_lock lock(table.mu);
  return table.frames.size();
}
