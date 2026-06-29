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

// vulkan.hpp's dynamic dispatcher is used here too; the single storage
// definition lives in device_caps.cc (linked alongside this TU).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "vulkan_drm_backend.h"

#include <drm_fourcc.h>
#include <drm_mode.h>  // DRM_MODE_ROTATE_*

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xf86drm.h>

#include <ctime>

#include <poll.h>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include "backend/drm_kms_egl/drm_cursor.h"
#include "backend/drm_kms_egl/scene_layer_source_vk.h"
#include "backend/drm_kms_vulkan/device_caps.h"
#include "backend/drm_kms_vulkan/drm_scanout_target.h"
#include "backend/drm_kms_vulkan/modifier_format.h"
#include "backend/drm_kms_vulkan/vulkan_backing_store.h"
#include "logging.h"

namespace {

// Accessor for the dynamic dispatcher (storage in device_caps.cc). A function
// rather than a namespace-scope reference so there is no static-init-order
// dependency on that other TU's global.
const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

// Map a rotation in degrees (validated to 0|90|180|270 at config time) to the
// KMS plane rotation bitflag. drm-cxx lowers DisplayParams::rotation to the
// plane's `rotation` property (or software pre-rotation if the plane lacks it).
uint64_t RotationToDrmFlag(const int degrees) {
  switch (degrees) {
    case 90:
      return DRM_MODE_ROTATE_90;
    case 180:
      return DRM_MODE_ROTATE_180;
    case 270:
      return DRM_MODE_ROTATE_270;
    default:
      return DRM_MODE_ROTATE_0;
  }
}

// Whether the display can scan out a buffer with this modifier under a 90/270
// rotation. amdgpu rejects both LINEAR (the rotated fetch needs a tiled read
// pattern) and DCC (delta-color compression is incompatible with the rotated
// read) for 90/270, so keep only tiled, non-DCC modifiers. Non-AMD tiled
// modifiers are assumed rotatable (the kernel still validates at AddFB/commit).
bool RotationCompatible(const uint64_t mod) {
  if (mod == DRM_FORMAT_MOD_LINEAR) {
    return false;
  }
#ifdef AMD_FMT_MOD
  if (static_cast<uint8_t>(mod >> 56) == DRM_FORMAT_MOD_VENDOR_AMD &&
      AMD_FMT_MOD_GET(DCC, mod) != 0U) {
    return false;
  }
#endif
  return true;
}

// Device extensions required for zero-copy dma-buf scanout and explicit
// synchronization. The dependencies of VK_EXT_image_drm_format_modifier
// (bind_memory2, get_memory_requirements2, sampler_ycbcr_conversion,
// get_physical_device_properties2) are all core in Vulkan 1.1, which the
// instance targets, so only the non-core extensions are listed here.
constexpr std::array<const char*, 7> kRequiredDeviceExtensions = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
};

bool HasExt(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& [extensionName, specVersion] : exts) {
    if (std::strcmp(extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

bool LooksLikeSoftware(const char* name) {
  return std::strstr(name, "llvmpipe") != nullptr ||
         std::strstr(name, "lavapipe") != nullptr ||
         std::strstr(name, "SwiftShader") != nullptr;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                   void* /*user_data*/) {
  const char* msg = data && data->pMessage ? data->pMessage : "";
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    spdlog::error("[vulkan] {}", msg);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    spdlog::warn("[vulkan] {}", msg);
  } else {
    spdlog::debug("[vulkan] {}", msg);
  }
  return VK_FALSE;
}

// Matches FlutterVulkanInstanceProcAddressCallback: the instance handle is an
// opaque void* (FlutterVulkanInstanceHandle), cast back to VkInstance here.
void* GetInstanceProcAddressCallback(void* /*user_data*/,
                                     void* instance,
                                     const char* procname) {
  return reinterpret_cast<void*>(
      d().vkGetInstanceProcAddr(static_cast<VkInstance>(instance), procname));
}

// Build a whole-color-aspect image-memory barrier.
VkImageMemoryBarrier ColorBarrier(VkImage image,
                                  VkImageLayout old_layout,
                                  VkImageLayout new_layout,
                                  VkAccessFlags src_access,
                                  VkAccessFlags dst_access) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = old_layout;
  b.newLayout = new_layout;
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return b;
}

// Record and submit one image-memory barrier on `queue`, blocking on `fence`
// until the GPU has executed it. Used to walk a backing-store image between the
// layout the Flutter Vulkan renderer leaves it in (COLOR_ATTACHMENT_OPTIMAL)
// and a flushed layout whose writes are visible to the KMS scanout engine.
void SubmitImageBarrier(VkDevice device,
                        VkCommandPool pool,
                        VkFence fence,
                        VkQueue queue,
                        const VkImageMemoryBarrier& barrier,
                        VkPipelineStageFlags src_stage,
                        VkPipelineStageFlags dst_stage) {
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (d().vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) {
    return;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d().vkBeginCommandBuffer(cmd, &bi);
  d().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                           1, &barrier);
  d().vkEndCommandBuffer(cmd);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  d().vkResetFences(device, 1, &fence);
  d().vkQueueSubmit(queue, 1, &si, fence);
  d().vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  d().vkFreeCommandBuffers(device, pool, 1, &cmd);
}

// One layer, many buffers: a LayerBufferSource that owns a ring of
// ExternalDmaBufSource framebuffers (one per scanout buffer) and presents
// whichever slot the backend marks ready for the current frame. This lets the
// engine render into a free buffer while KMS scans another — the basis for
// tear-free, vsync-paced double/triple buffering on a single plane.
class VkScanoutRing final : public drm::scene::LayerBufferSource {
 public:
  // Import `store`'s dma-buf as a KMS framebuffer and append it as a ring slot.
  // Returns the new slot index, or nullopt if the framebuffer import failed.
  std::optional<size_t> AddSlot(const drm::Device& dev,
                                const drm_kms_vulkan::VulkanBackingStore& store,
                                uint32_t fourcc) {
    std::vector<drm::scene::ExternalPlaneInfo> planes;
    for (const auto& pl : store.planes()) {
      drm::scene::ExternalPlaneInfo info{};
      info.fd = store.dma_buf_fd();
      info.offset = static_cast<uint32_t>(pl.offset);
      info.pitch = static_cast<uint32_t>(pl.pitch);
      planes.push_back(info);
    }
    // Import the FB with the buffer's ACTUAL modifier (LINEAR or a tiled AMD
    // swizzle), not a hard-coded LINEAR — drm-cxx forwards it verbatim to
    // drmModeAddFB2WithModifiers and the kernel validates the tiling. A tiled
    // layout is what amdgpu requires to scan out a 90/270-rotated plane.
    const uint64_t mod = store.modifier();
    auto src = drm::scene::ExternalDmaBufSource::create(
        dev, store.width(), store.height(), fourcc, mod, planes);
    if (!src) {
      spdlog::error(
          "[VulkanDrmBackend] framebuffer import ({}x{} fourcc=0x{:08x} "
          "mod=0x{:016x} planes={} offset={} pitch={}): {}",
          store.width(), store.height(), fourcc, mod, planes.size(),
          planes.empty() ? 0u : planes[0].offset,
          planes.empty() ? 0u : planes[0].pitch, src.error().message());
      return std::nullopt;
    }
    slots_.push_back(std::move(src.value()));
    return slots_.size() - 1;
  }

  void SetReady(const size_t slot) noexcept { ready_ = slot; }
  [[nodiscard]] size_t slot_count() const noexcept { return slots_.size(); }

  // ── LayerBufferSource — forwarded to the ready slot. ──────────────────────
  [[nodiscard]] drm::expected<drm::scene::AcquiredBuffer, std::error_code>
  acquire() override {
    last_acquired_ = ready_;
    return slots_[ready_]->acquire();
  }
  void release(drm::scene::AcquiredBuffer acquired) noexcept override {
    slots_[last_acquired_]->release(std::move(acquired));
  }
  [[nodiscard]] drm::scene::BindingModel binding_model()
      const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override {
    return slots_.empty() ? drm::scene::SourceFormat{}
                          : slots_[ready_]->format();
  }
  void on_session_paused() noexcept override {
    for (auto& s : slots_) {
      s->on_session_paused();
    }
  }
  drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override {
    for (auto& s : slots_) {
      auto r = s->on_session_resumed(new_dev);
      if (!r) {
        return r;
      }
    }
    return {};
  }

 private:
  std::vector<std::unique_ptr<drm::scene::ExternalDmaBufSource>> slots_;
  size_t ready_ = 0;
  size_t last_acquired_ = 0;
};

// Block until one queued page-flip event drains from the DRM fd, pacing the
// caller to vblank. Bounded by a timeout so a missed event degrades to a
// dropped frame instead of a hang.
void WaitForFlip(const int drm_fd, drmEventContext& evctx) {
  struct pollfd pfd{drm_fd, POLLIN, 0};
  if (::poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN) != 0) {
    drmHandleEvent(drm_fd, &evctx);
  }
}

}  // namespace

VulkanDrmBackend::VulkanDrmBackend(std::string drm_device,
                                   const bool enable_validation,
                                   homescreen::DrmSession* session,
                                   std::string mode_spec,
                                   const int rotation)
    : drm_device_(std::move(drm_device)),
      mode_spec_(std::move(mode_spec)),
      rotation_(rotation),
      enable_validation_(enable_validation),
      session_(session) {}

VulkanDrmBackend::~VulkanDrmBackend() {
  // Cadence profile summary (no-op unless IVI_PROFILE / IVI_DRMVK_PROFILE ran).
  frame_profile_.LogSessionSummary("VulkanDrmBackend");
  // Tear the cursor down first: it commits on compositor_'s DRM device, so it
  // must not outlive it.
  cursor_.reset();
  // Drop master + scene + backing stores while the Vulkan device is still
  // alive (the stores free Vulkan resources in their destructors).
  compositor_.reset();
  Teardown();
}

std::shared_ptr<VulkanDrmBackend> VulkanDrmBackend::Create(
    const std::string& drm_device,
    const bool enable_validation,
    homescreen::DrmSession* session,
    const std::string& mode_spec,
    const int rotation) {
  auto backend = std::shared_ptr<VulkanDrmBackend>(new VulkanDrmBackend(
      drm_device, enable_validation, session, mode_spec, rotation));

  std::string err;
  if (!backend->BringUp(err)) {
    spdlog::critical("[VulkanDrmBackend] init failed; refusing to start: {}",
                     err);
    return nullptr;
  }
  if (!backend->SetupCompositor(err)) {
    spdlog::critical(
        "[VulkanDrmBackend] compositor setup failed; refusing to start: {}",
        err);
    return nullptr;
  }
  return backend;
}

bool VulkanDrmBackend::BringUp(std::string& refusal_reason) {
  try {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();
  } catch (const std::exception& e) {
    refusal_reason = std::string(
                         "Vulkan loader not present (libvulkan could not be "
                         "opened): ") +
                     e.what();
    return false;
  }
  if (d().vkCreateInstance == nullptr) {
    refusal_reason = "Vulkan loader present but vkCreateInstance unresolved";
    return false;
  }

  if (!CreateInstance(refusal_reason)) {
    return false;
  }
  SetupDebugMessenger();
  if (!SelectPhysicalDevice(refusal_reason)) {
    return false;
  }
  if (!CreateLogicalDevice(refusal_reason)) {
    return false;
  }
  PopulateCaps();

  spdlog::info(
      "[VulkanDrmBackend] device='{}' driver='{}' vendor=0x{:04x} "
      "device=0x{:04x} api={}.{}.{}",
      caps_.device_name, caps_.driver_name, caps_.vendor_id, caps_.device_id,
      VK_VERSION_MAJOR(caps_.api_version), VK_VERSION_MINOR(caps_.api_version),
      VK_VERSION_PATCH(caps_.api_version));
  spdlog::info(
      "[VulkanDrmBackend] caps: drm_node={} timeline_sem={} global_priority={} "
      "lazy_transient={} dedicated_transfer={} gfx_queues={} max_image_2d={}",
      caps_.has_physical_device_drm, caps_.has_timeline_semaphore,
      caps_.has_global_priority, caps_.has_lazy_transient,
      caps_.has_dedicated_transfer_queue, caps_.graphics_queue_count,
      caps_.max_image_2d);
  spdlog::info(
      "[VulkanDrmBackend] graphics queue family {} created; scanout node '{}'",
      graphics_queue_family_, drm_device_);

  return true;
}

// ── Compositor (present path)
// ─────────────────────────────────────────────────

struct VulkanDrmBackend::CompositorState {
  explicit CompositorState(drm::Device d) : device(std::move(d)) {}
  ~CompositorState() {
    // Drain the in-flight flip so KMS is not mid-page-flip on a buffer we are
    // about to free, then idle the GPU before releasing any Vulkan resources.
    if (flip_pending) {
      WaitForFlip(device.fd(), evctx);
    }
    if (vk_device != VK_NULL_HANDLE) {
      d().vkDeviceWaitIdle(vk_device);
    }
    // Order matters: drop the scene (and the ring source's framebuffers) before
    // freeing the images/dma-bufs the framebuffers reference.
    scene.reset();
    ring_owner.reset();
    slots.clear();
    if (barrier_fence != VK_NULL_HANDLE) {
      d().vkDestroyFence(vk_device, barrier_fence, nullptr);
    }
    if (barrier_pool != VK_NULL_HANDLE) {
      d().vkDestroyCommandPool(vk_device, barrier_pool, nullptr);
    }
    if (have_master) {
      drmDropMaster(device.fd());
    }
  }

  drm::Device device;
  std::unique_ptr<drm::scene::LayerScene> scene;
  bool have_master = false;
  uint32_t fourcc = 0;
  // Render extent: the backing-store / Flutter-viewport size. For a 90/270
  // rotation this is the CRTC mode with width/height swapped (the GPU renders
  // landscape into a buffer the plane rotates onto a portrait panel).
  uint32_t width = 0;
  uint32_t height = 0;
  // Scanout extent: the CRTC mode (the plane's dst_rect). Equals width/height
  // when rotation is 0/180.
  uint32_t crtc_width = 0;
  uint32_t crtc_height = 0;
  // Plane rotation (DRM_MODE_ROTATE_*) applied to the scanned-out buffer.
  uint64_t rotation = 0;
  // Modifiers the backing stores allocate against. LINEAR for an unrotated
  // scanout (simple, universally importable); tiled non-DCC for 90/270 (amdgpu
  // requires a tiled layout to rotate, and rejects DCC + rotation).
  std::vector<uint64_t> scanout_modifiers;

  // Ring of scanout buffers. The engine cycles through them
  // (avoid_backing_store_cache), rendering into a free slot while KMS scans
  // another; a single persistent layer presents whichever slot is ready.
  static constexpr size_t kMaxRing = 6;
  struct Slot {
    std::unique_ptr<drm_kms_vulkan::VulkanBackingStore> store;
    bool engine_owned = false;  // handed to the engine, not yet collected
  };
  std::vector<Slot> slots;
  std::unordered_map<const void*, size_t> key_to_slot;
  std::unique_ptr<VkScanoutRing>
      ring_owner;                 // until moved into the scene layer
  VkScanoutRing* ring = nullptr;  // stable; owned by the scene layer
  std::optional<drm::scene::LayerHandle> layer;

  // Vsync pacing. The first commit is a blocking modeset; subsequent commits
  // are non-blocking page flips drained against this event context.
  bool first_commit = true;
  bool flip_pending = false;
  int scanning_slot = -1;  // slot the CRTC is currently scanning
  int pending_slot = -1;   // slot in the in-flight non-blocking flip
  drmEventContext evctx{};

  // Scanout hand-off: drives each backing-store image's layout/cache between
  // the renderer and the KMS scanout engine.
  VkDevice vk_device = VK_NULL_HANDLE;
  VkCommandPool barrier_pool = VK_NULL_HANDLE;
  VkFence barrier_fence = VK_NULL_HANDLE;
  uint64_t frame = 0;
};

bool VulkanDrmBackend::SetupCompositor(std::string& err) {
  drm_kms_vulkan::ScanoutTarget target;
  if (!drm_kms_vulkan::DiscoverScanoutTarget(drm_device_, DRM_FORMAT_XRGB8888,
                                             mode_spec_, target, err)) {
    return false;
  }
  const std::vector<uint64_t> allowed = drm_kms_vulkan::NegotiateModifiers(
      physical_device_, VK_FORMAT_B8G8R8A8_UNORM, target.plane_modifiers);
  if (allowed.empty()) {
    err = "no modifier common to the GPU and the scanout plane";
    return false;
  }
  spdlog::info(
      "[VulkanDrmBackend] scanout target: connector {} crtc {} plane {} mode "
      "{}x{}; {} modifier(s) negotiated",
      target.connector_id, target.crtc_id, target.primary_plane_id,
      target.mode_width, target.mode_height, allowed.size());
  {
    std::string adv;
    for (auto m : target.plane_modifiers) {
      adv += " " + drm_kms_vulkan::DescribeModifier(m);
    }
    std::string neg;
    for (auto m : allowed) {
      neg += " " + drm_kms_vulkan::DescribeModifier(m);
    }
    spdlog::info("[VulkanDrmBackend] plane modifiers advertised:{}", adv);
    spdlog::info("[VulkanDrmBackend] modifiers negotiated:{}", neg);
  }

  auto dev_exp = drm::Device::open(drm_device_);
  if (!dev_exp) {
    err = "drm::Device::open: " + dev_exp.error().message();
    return false;
  }
  auto state = std::make_unique<CompositorState>(std::move(dev_exp.value()));
  (void)state->device.enable_atomic();
  (void)state->device.enable_universal_planes();
  state->fourcc = DRM_FORMAT_XRGB8888;
  // The CRTC always scans its native mode; a 90/270 rotation swaps the render
  // extent (backing stores + Flutter viewport) so the GPU paints landscape into
  // a buffer the plane rotates onto the portrait panel. 0/180 keep the extents
  // equal.
  state->crtc_width = target.mode_width;
  state->crtc_height = target.mode_height;
  const bool swap = (rotation_ == 90 || rotation_ == 270);
  state->width = swap ? target.mode_height : target.mode_width;
  state->height = swap ? target.mode_width : target.mode_height;
  state->rotation = RotationToDrmFlag(rotation_);

  // Pick the backing-store modifiers. Unrotated: LINEAR (simple, importable
  // everywhere). 90/270: a tiled non-DCC modifier from the negotiated set —
  // amdgpu needs tiling to rotate and rejects DCC + rotation. If the filter
  // empties (no tiled scanout modifier), fall back to LINEAR so allocation
  // still succeeds; the rotated commit then fails loudly rather than silently.
  if (swap) {
    for (const auto m : allowed) {
      if (RotationCompatible(m)) {
        state->scanout_modifiers.push_back(m);
      }
    }
    if (state->scanout_modifiers.empty()) {
      spdlog::warn(
          "[VulkanDrmBackend] no tiled non-DCC modifier available for a "
          "{}-degree rotation; the rotated scanout will not commit",
          rotation_);
      state->scanout_modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
    } else {
      std::string picked;
      for (const auto m : state->scanout_modifiers) {
        picked += " " + drm_kms_vulkan::DescribeModifier(m);
      }
      spdlog::info("[VulkanDrmBackend] rotation {} backing-store modifiers:{}",
                   rotation_, picked);
    }
  } else {
    state->scanout_modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
  }

  drm::scene::LayerScene::Config scfg{};
  scfg.crtc_id = target.crtc_id;
  scfg.connector_id = target.connector_id;
  scfg.mode = target.mode;
  auto scene_exp = drm::scene::LayerScene::create(state->device, scfg);
  if (!scene_exp) {
    err = "LayerScene::create: " + scene_exp.error().message();
    return false;
  }
  state->scene = std::move(scene_exp.value());

  // Probe the scanout path once: allocate a mode-sized backing store and try to
  // import it as a KMS framebuffer. On hardware whose display cannot address
  // the render GPU's exported buffer (e.g. a split render/display SoC with no
  // display IOMMU — RPi4 vc4), this fails here, so refuse cleanly now instead
  // of letting every per-frame CreateBackingStore fail into a black screen. The
  // probe image and its framebuffer are released immediately (ring before
  // store, so the framebuffer is gone before the memory it references).
  {
    std::string probe_err;
    auto probe = drm_kms_vulkan::VulkanBackingStore::Create(
        physical_device_, device_, state->width, state->height,
        VK_FORMAT_B8G8R8A8_UNORM, state->fourcc, state->scanout_modifiers,
        probe_err);
    if (!probe) {
      err = "scanout buffer allocation failed: " + probe_err;
      return false;
    }
    if (VkScanoutRing probe_ring;
        !probe_ring.AddSlot(state->device, *probe, state->fourcc)) {
      err =
          "this GPU/display combination cannot zero-copy scan out the Vulkan "
          "renderer's buffers (KMS framebuffer import failed) — use the GL "
          "backend (drm_kms_egl) on this hardware";
      return false;
    }
  }

  // Command pool + fence for the per-frame scanout hand-off barriers.
  state->vk_device = device_;
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = graphics_queue_family_;
  if (d().vkCreateCommandPool(device_, &pci, nullptr, &state->barrier_pool) !=
      VK_SUCCESS) {
    err = "vkCreateCommandPool for scanout hand-off failed";
    return false;
  }
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (d().vkCreateFence(device_, &fci, nullptr, &state->barrier_fence) !=
      VK_SUCCESS) {
    err = "vkCreateFence for scanout hand-off failed";
    return false;
  }

  // Page-flip event draining for vsync pacing. A no-op handler is enough — the
  // event arriving is the signal; we carry no per-flip state through it.
  state->evctx.version = 2;
  state->evctx.page_flip_handler =
      [](int /*fd*/, unsigned int /*sequence*/, unsigned int /*tv_sec*/,
         unsigned int /*tv_usec*/, void* /*user_data*/) {};

  if (drmSetMaster(state->device.fd()) != 0) {
    err = std::string("drmSetMaster: ") + std::strerror(errno) +
          " (run from a free VT / stop the active compositor)";
    return false;
  }
  state->have_master = true;

  width_ = state->width;
  height_ = state->height;
  compositor_ = std::move(state);
  spdlog::info(
      "[VulkanDrmBackend] compositor ready: {}x{}, DRM master acquired", width_,
      height_);

#if HAVE_DRM_CURSOR
  // Self-committing HW cursor on the scanout CRTC's cursor plane. It commits on
  // its own (driven by seat pointer motion), NOT staged into the per-frame
  // scanout commit, so it stays responsive while the UI is idle. Failure is
  // non-fatal — the shell runs without an on-screen sprite. Built on
  // compositor_'s DRM device (master held above), so it lives in cursor_ and is
  // torn down before compositor_.
  // The cursor plane lives on the CRTC, so its bounds are the panel (mode)
  // size, not the (possibly rotation-swapped) render size. The seat transforms
  // pointer positions from render space into this panel space before placing
  // the sprite.
  cursor_ = homescreen::DrmCursor::Create(
      compositor_->device, target.crtc_id, target.connector_id, target.mode,
      target.mode_width, target.mode_height, rotation_);
  if (!cursor_) {
    spdlog::info("[VulkanDrmBackend] no HW cursor (disabled or unavailable)");
  }
#endif
  return true;
}

FlutterVulkanImage VulkanDrmBackend::GetNextImageCb(
    void* /*user_data*/,
    const FlutterFrameInfo* /*frame_info*/) {
  // Unreachable on the compositor path; present a well-formed but empty image
  // so the embedder's renderer-config validation is satisfied.
  FlutterVulkanImage img{};
  img.struct_size = sizeof(FlutterVulkanImage);
  img.image = 0;  // FlutterVulkanImageHandle is an opaque uint64, not a pointer
  img.format = VK_FORMAT_B8G8R8A8_UNORM;
  return img;
}

bool VulkanDrmBackend::PresentImageCb(void* /*user_data*/,
                                      const FlutterVulkanImage* /*image*/) {
  return true;
}

bool VulkanDrmBackend::CreateBackingStoreCb(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* out,
    void* user_data) {
  return static_cast<VulkanDrmBackend*>(user_data)->CreateBackingStoreImpl(
      config, out);
}

bool VulkanDrmBackend::CollectBackingStoreCb(const FlutterBackingStore* store,
                                             void* user_data) {
  return static_cast<VulkanDrmBackend*>(user_data)->CollectBackingStoreImpl(
      store);
}

bool VulkanDrmBackend::PresentLayersCb(const FlutterLayer** layers,
                                       size_t count,
                                       void* user_data) {
  return static_cast<VulkanDrmBackend*>(user_data)->PresentLayersImpl(layers,
                                                                      count);
}

bool VulkanDrmBackend::CreateBackingStoreImpl(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* out) {
  if (!compositor_) {
    return false;
  }
  CompositorState& c = *compositor_;
  const auto w = static_cast<uint32_t>(config->size.width);
  const auto h = static_cast<uint32_t>(config->size.height);

  // Reuse a ring slot that the engine has released and the scanout engine is no
  // longer scanning or about to scan; otherwise grow the ring.
  int slot = -1;
  for (size_t i = 0; i < c.slots.size(); ++i) {
    if (const auto& [store, engine_owned] = c.slots[i];
        !engine_owned && static_cast<int>(i) != c.scanning_slot &&
        static_cast<int>(i) != c.pending_slot && store->width() == w &&
        store->height() == h) {
      slot = static_cast<int>(i);
      break;
    }
  }

  if (slot < 0) {
    if (c.slots.size() >= CompositorState::kMaxRing) {
      spdlog::error(
          "[VulkanDrmBackend] scanout ring exhausted ({} buffers); dropping "
          "frame",
          c.slots.size());
      return false;
    }
    std::string err;
    // Allocate against the negotiated modifier set chosen in SetupCompositor:
    // LINEAR when unrotated, a tiled non-DCC modifier for 90/270. The driver
    // picks one and exports its per-plane layout; AddSlot imports the FB with
    // store.modifier() so the tiling is honored end to end.
    auto store = drm_kms_vulkan::VulkanBackingStore::Create(
        physical_device_, device_, w, h, VK_FORMAT_B8G8R8A8_UNORM, c.fourcc,
        c.scanout_modifiers, err);
    if (!store) {
      spdlog::error("[VulkanDrmBackend] CreateBackingStore({}x{}): {}", w, h,
                    err);
      return false;
    }
    if (c.ring == nullptr) {
      c.ring_owner = std::make_unique<VkScanoutRing>();
      c.ring = c.ring_owner.get();
    }
    auto idx = c.ring->AddSlot(c.device, *store, c.fourcc);
    if (!idx) {
      spdlog::error("[VulkanDrmBackend] scanout framebuffer import failed");
      return false;
    }
    slot = static_cast<int>(*idx);
    c.key_to_slot[store.get()] = static_cast<size_t>(slot);
    c.slots.push_back({std::move(store), false});
    spdlog::info("[VulkanDrmBackend] scanout ring grew to {} buffer(s) ({}x{})",
                 c.slots.size(), w, h);
  }

  auto& [store, engine_owned] = c.slots[static_cast<size_t>(slot)];
  engine_owned = true;
  // Hand the engine an image in the layout it renders into. UNDEFINED as the
  // old layout discards the slot's prior contents — the engine fully repaints
  // each frame (avoid_backing_store_cache), so nothing is lost.
  SubmitImageBarrier(device_, c.barrier_pool, c.barrier_fence, graphics_queue_,
                     ColorBarrier(store->image(), VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  const void* key = store.get();
  out->struct_size = sizeof(FlutterBackingStore);
  out->type = kFlutterBackingStoreTypeVulkan;
  out->user_data = const_cast<void*>(key);
  out->vulkan.struct_size = sizeof(FlutterVulkanBackingStore);
  out->vulkan.image = store->flutter_image();
  out->vulkan.user_data = const_cast<void*>(key);
  out->vulkan.destruction_callback = [](void*) {};
  return true;
}

bool VulkanDrmBackend::CollectBackingStoreImpl(
    const FlutterBackingStore* store) {
  if (!compositor_) {
    return false;
  }
  // Return the slot to the free pool; its image/framebuffer stay alive for
  // reuse and are released with the whole ring at teardown.
  if (const auto it = compositor_->key_to_slot.find(store->user_data);
      it != compositor_->key_to_slot.end()) {
    compositor_->slots[it->second].engine_owned = false;
  }
  return true;
}

bool VulkanDrmBackend::PresentLayersImpl(const FlutterLayer** layers,
                                         size_t count) {
  if (!compositor_ || !compositor_->scene) {
    return false;
  }
  CompositorState& c = *compositor_;
  const FlutterLayer* bs_layer = nullptr;
  for (size_t i = 0; i < count; ++i) {
    if (layers[i]->type == kFlutterLayerContentTypeBackingStore) {
      bs_layer = layers[i];
      break;
    }
  }
  if (bs_layer == nullptr) {
    return false;
  }
  const auto it = c.key_to_slot.find(bs_layer->backing_store->user_data);
  if (it == c.key_to_slot.end()) {
    return false;
  }
  const size_t slot = it->second;
  drm_kms_vulkan::VulkanBackingStore* store = c.slots[slot].store.get();

  // Flush the renderer's color writes so the scanout engine sees this frame.
  // The embedder host-syncs all layers before present_layers, so the render is
  // already retired; this barrier only makes the writes visible to KMS.
  SubmitImageBarrier(
      device_, c.barrier_pool, c.barrier_fence, graphics_queue_,
      ColorBarrier(store->image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_MEMORY_READ_BIT),
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

  // Pace to vblank: drain the previous non-blocking flip before queueing the
  // next. The drained flip's slot becomes the one being scanned; the slot it
  // replaced returns to the free pool.
  if (c.flip_pending) {
    WaitForFlip(c.device.fd(), c.evctx);
    c.flip_pending = false;
    c.scanning_slot = c.pending_slot;
    c.pending_slot = -1;
  }

  // Create the single presenting layer once the ring has a slot; thereafter the
  // ring source rotates which slot's framebuffer the layer scans out.
  if (!c.layer) {
    if (c.ring == nullptr) {
      return false;
    }
    drm::scene::LayerDesc desc;
    desc.source = std::move(c.ring_owner);
    // dst_rect is the CRTC mode; src_rect defaults to the buffer's full extent
    // (the render size). For a 90/270 rotation the render buffer is landscape
    // and the plane rotates it onto the portrait CRTC, so dst != src extent.
    desc.display.dst_rect = {0, 0, c.crtc_width, c.crtc_height};
    desc.display.rotation = c.rotation;
    auto layer = c.scene->add_layer(std::move(desc));
    if (!layer) {
      spdlog::error("[VulkanDrmBackend] add_layer: {}",
                    layer.error().message());
      return false;
    }
    c.layer = layer.value();
  }

  c.ring->SetReady(slot);
  // First commit is a blocking modeset (the scene adds ALLOW_MODESET, which
  // cannot be non-blocking); subsequent frames are non-blocking page flips we
  // pace against.
  const uint32_t flags =
      c.first_commit ? 0U
                     : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);
  if (auto report = c.scene->commit(flags); !report) {
    spdlog::error("[VulkanDrmBackend] commit: {}", report.error().message());
    return false;
  }
  if (c.first_commit) {
    c.first_commit = false;
    c.scanning_slot = static_cast<int>(slot);
  } else {
    c.pending_slot = static_cast<int>(slot);
    c.flip_pending = true;
  }

  const uint64_t n = c.frame++;
  if (n < 3 || n % 120 == 0) {
    spdlog::info(
        "[VulkanDrmBackend] presented frame {} slot {} ({}x{}); ring={}", n,
        slot, store->width(), store->height(), c.slots.size());
  }

  static const bool profile_enabled =
      profiling::FrameProfile::Enabled("IVI_DRMVK_PROFILE");
  if (profile_enabled) {
    frame_profile_.Record("VulkanDrmBackend", /*ok=*/true);
  }
  return true;
}

bool VulkanDrmBackend::CreateInstance(std::string& refusal_reason) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "ivi-homescreen";
  app.apiVersion = VK_API_VERSION_1_1;

  if (enable_validation_) {
    uint32_t layer_count = 0;
    d().vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    if (layer_count > 0) {
      d().vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    }
    constexpr const char* kValidation = "VK_LAYER_KHRONOS_validation";
    for (const auto& l : layers) {
      if (std::strcmp(l.layerName, kValidation) == 0) {
        enabled_instance_layers_.push_back(kValidation);
        enabled_instance_extensions_.push_back(
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        break;
      }
    }
    if (enabled_instance_layers_.empty()) {
      spdlog::warn(
          "[VulkanDrmBackend] validation requested (-d) but "
          "VK_LAYER_KHRONOS_validation is not enumerable; continuing without "
          "validation");
    }
  }

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  info.enabledLayerCount =
      static_cast<uint32_t>(enabled_instance_layers_.size());
  info.ppEnabledLayerNames = enabled_instance_layers_.data();
  info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_instance_extensions_.size());
  info.ppEnabledExtensionNames = enabled_instance_extensions_.data();

  if (d().vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
    refusal_reason = "vkCreateInstance failed";
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
  return true;
}

void VulkanDrmBackend::SetupDebugMessenger() {
  if (enabled_instance_layers_.empty() ||
      d().vkCreateDebugUtilsMessengerEXT == nullptr) {
    return;
  }
  VkDebugUtilsMessengerCreateInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = DebugUtilsCallback;
  d().vkCreateDebugUtilsMessengerEXT(instance_, &info, nullptr,
                                     &debug_messenger_);
}

bool VulkanDrmBackend::SelectPhysicalDevice(std::string& refusal_reason) {
  uint32_t count = 0;
  d().vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count > 0) {
    d().vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  }

  unsigned disp_major = 0;
  unsigned disp_minor = 0;
  drm_kms_vulkan::DrmNodeNumber(drm_device_, disp_major, disp_minor);

  uint64_t best_score = 0;
  std::string last_miss;
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props{};
    d().vkGetPhysicalDeviceProperties(pd, &props);

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        LooksLikeSoftware(props.deviceName)) {
      last_miss = std::string(props.deviceName) + " is a CPU/software renderer";
      continue;
    }

    // The Flutter engine's Skia backend requires a Vulkan 1.1 device. Reject a
    // 1.0 device here (e.g. some virtio/gfxstream Turnip stacks expose only
    // 1.0) so the backend refuses with a clear reason instead of the engine
    // fatally aborting during renderer init.
    if (props.apiVersion < VK_API_VERSION_1_1) {
      last_miss = std::string(props.deviceName) + " exposes Vulkan " +
                  std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
                  std::to_string(VK_VERSION_MINOR(props.apiVersion)) +
                  " (the Flutter Vulkan renderer requires 1.1)";
      continue;
    }

    uint32_t ext_count = 0;
    d().vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    if (ext_count > 0) {
      d().vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count,
                                               exts.data());
    }
    bool has_all = true;
    for (const char* req : kRequiredDeviceExtensions) {
      if (!HasExt(exts, req)) {
        has_all = false;
        last_miss = std::string(props.deviceName) + " missing " + req;
        break;
      }
    }
    if (!has_all) {
      continue;
    }

    uint32_t qf_count = 0;
    d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    if (qf_count > 0) {
      d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
    }
    uint32_t gfx_family = UINT32_MAX;
    for (uint32_t i = 0; i < qfs.size(); ++i) {
      if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        gfx_family = i;
        break;
      }
    }
    if (gfx_family == UINT32_MAX) {
      last_miss = std::string(props.deviceName) + " has no graphics queue";
      continue;
    }

    // A DRM-node match against the scanout device dominates (render on the GPU
    // that drives the display); then prefer discrete GPUs; then a larger max
    // image dimension.
    uint64_t score = 1;
    if (HasExt(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) &&
        (disp_major != 0 || disp_minor != 0)) {
      VkPhysicalDeviceDrmPropertiesEXT drm{};
      drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
      VkPhysicalDeviceProperties2 p2{};
      p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p2.pNext = &drm;
      d().vkGetPhysicalDeviceProperties2(pd, &p2);
      const bool primary_match =
          drm.hasPrimary &&
          static_cast<unsigned>(drm.primaryMajor) == disp_major &&
          static_cast<unsigned>(drm.primaryMinor) == disp_minor;
      const bool render_match =
          drm.hasRender &&
          static_cast<unsigned>(drm.renderMajor) == disp_major &&
          static_cast<unsigned>(drm.renderMinor) == disp_minor;
      if (primary_match || render_match) {
        score += 1ULL << 40;
      }
    }
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score += 1ULL << 30;
    }
    score += props.limits.maxImageDimension2D;

    if (score > best_score) {
      best_score = score;
      physical_device_ = pd;
      graphics_queue_family_ = gfx_family;
      enabled_device_extensions_.assign(kRequiredDeviceExtensions.begin(),
                                        kRequiredDeviceExtensions.end());
    }
  }

  if (physical_device_ == VK_NULL_HANDLE) {
    refusal_reason =
        "no Vulkan physical device supports zero-copy dma-buf scanout" +
        (last_miss.empty() ? std::string()
                           : std::string(" (") + last_miss + ")");
    return false;
  }
  return true;
}

bool VulkanDrmBackend::CreateLogicalDevice(std::string& refusal_reason) {
  // Query which optional sync features the selected device offers so the
  // feature chain only enables what is present.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_supported{};
  timeline_supported.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  VkPhysicalDeviceSynchronization2Features sync2_supported{};
  sync2_supported.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2_supported.pNext = &timeline_supported;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &sync2_supported;
  d().vkGetPhysicalDeviceFeatures2(physical_device_, &features2);

  if (sync2_supported.synchronization2 != VK_TRUE) {
    refusal_reason = "selected device does not support synchronization2";
    return false;
  }

  VkPhysicalDeviceSynchronization2Features sync2_enable{};
  sync2_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2_enable.synchronization2 = VK_TRUE;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_enable{};
  timeline_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  timeline_enable.timelineSemaphore = VK_TRUE;
  if (timeline_supported.timelineSemaphore == VK_TRUE) {
    sync2_enable.pNext = &timeline_enable;
  }

  constexpr float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = graphics_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures device_features{};
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.pNext = &sync2_enable;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();
  device_info.pEnabledFeatures = &device_features;

  if (d().vkCreateDevice(physical_device_, &device_info, nullptr, &device_) !=
      VK_SUCCESS) {
    refusal_reason = "vkCreateDevice failed";
    return false;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));
  d().vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
  return true;
}

void VulkanDrmBackend::PopulateCaps() {
  VkPhysicalDeviceProperties props{};
  d().vkGetPhysicalDeviceProperties(physical_device_, &props);
  caps_.device_name = props.deviceName;
  caps_.vendor_id = props.vendorID;
  caps_.device_id = props.deviceID;
  caps_.api_version = props.apiVersion;
  caps_.max_image_2d = props.limits.maxImageDimension2D;

  uint32_t ext_count = 0;
  d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                           &ext_count, nullptr);
  std::vector<VkExtensionProperties> exts(ext_count);
  if (ext_count > 0) {
    d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                             &ext_count, exts.data());
  }
  caps_.has_physical_device_drm =
      HasExt(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
  caps_.has_global_priority = HasExt(exts, "VK_EXT_global_priority") ||
                              HasExt(exts, "VK_KHR_global_priority");

  // Query the actual timelineSemaphore feature bit rather than inferring it
  // from the API version or extension list — Mesa Turnip, for one, exposes it
  // on a device whose advertised properties would not imply it.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
  timeline.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  VkPhysicalDeviceFeatures2 timeline_features2{};
  timeline_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  timeline_features2.pNext = &timeline;
  d().vkGetPhysicalDeviceFeatures2(physical_device_, &timeline_features2);
  caps_.has_timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;

  VkPhysicalDeviceDriverProperties driver{};
  driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  p2.pNext = &driver;
  d().vkGetPhysicalDeviceProperties2(physical_device_, &p2);
  caps_.driver_name = driver.driverName;

  VkPhysicalDeviceMemoryProperties mem{};
  d().vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if (mem.memoryTypes[i].propertyFlags &
        VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
      caps_.has_lazy_transient = true;
      break;
    }
  }

  uint32_t qf_count = 0;
  d().vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count,
                                               nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  if (qf_count > 0) {
    d().vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count,
                                                 qfs.data());
  }
  if (graphics_queue_family_ < qfs.size()) {
    caps_.graphics_queue_count = qfs[graphics_queue_family_].queueCount;
  }
  for (const auto& qf : qfs) {
    const bool transfer = (qf.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
    const bool graphics = (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    const bool compute = (qf.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    if (transfer && !graphics && !compute) {
      caps_.has_dedicated_transfer_queue = true;
      break;
    }
  }

  caps_.zero_copy_supported = true;
}

void VulkanDrmBackend::Teardown() {
  if (device_ != VK_NULL_HANDLE) {
    d().vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (debug_messenger_ != VK_NULL_HANDLE &&
      d().vkDestroyDebugUtilsMessengerEXT != nullptr) {
    d().vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    debug_messenger_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    d().vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

// ── Backend interface ──────────────────────────────────────────────────────
// The Flutter Vulkan renderer drives a fixed-size scanout target, so resize is
// driven by the discovered mode rather than these call sites; they satisfy the
// vtable and the FlutterView wiring.

void VulkanDrmBackend::Resize(size_t /*index*/,
                              Engine* /*flutter_engine*/,
                              int32_t /*width*/,
                              int32_t /*height*/) {}

void VulkanDrmBackend::CreateSurface(size_t /*index*/,
                                     struct wl_surface* /*surface*/,
                                     int32_t /*width*/,
                                     int32_t /*height*/) {}

bool VulkanDrmBackend::TextureMakeCurrent() {
  return false;
}

bool VulkanDrmBackend::TextureClearCurrent() {
  return false;
}

FlutterRendererConfig VulkanDrmBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kVulkan;
  config.vulkan.struct_size = sizeof(FlutterVulkanRendererConfig);
  config.vulkan.version = VK_MAKE_VERSION(1, 1, 0);
  config.vulkan.instance = instance_;
  config.vulkan.physical_device = physical_device_;
  config.vulkan.device = device_;
  config.vulkan.queue_family_index = graphics_queue_family_;
  config.vulkan.queue = graphics_queue_;
  config.vulkan.enabled_instance_extension_count =
      enabled_instance_extensions_.size();
  config.vulkan.enabled_instance_extensions =
      enabled_instance_extensions_.data();
  config.vulkan.enabled_device_extension_count =
      enabled_device_extensions_.size();
  config.vulkan.enabled_device_extensions = enabled_device_extensions_.data();
  config.vulkan.get_instance_proc_address_callback =
      GetInstanceProcAddressCallback;
  config.vulkan.get_next_image_callback = GetNextImageCb;
  config.vulkan.present_image_callback = PresentImageCb;
  return config;
}

FlutterCompositor VulkanDrmBackend::GetCompositorConfig() {
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  compositor.user_data = this;
  compositor.create_backing_store_callback = CreateBackingStoreCb;
  compositor.collect_backing_store_callback = CollectBackingStoreCb;
  compositor.present_layers_callback = PresentLayersCb;
  // Force a fresh backing store each frame so the engine cycles through the
  // scanout ring (rendering into a free buffer while KMS scans another) rather
  // than reusing one buffer — the basis for tear-free, vsync-paced present.
  compositor.avoid_backing_store_cache = true;
  return compositor;
}
