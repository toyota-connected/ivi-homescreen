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

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xf86drm.h>

#include <ctime>

#include <poll.h>
#include <unistd.h>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/external_dma_buf_pool.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/sync/fence.hpp>

#include "backend/drm_kms_egl/drm_cursor.h"
#include "backend/drm_kms_egl/scene_layer_source_vk.h"
#if BUILD_HUD
#include "backend/hud/vulkan_hud.h"
#endif
#include "backend/drm_kms_vulkan/device_caps.h"
#include "backend/drm_kms_vulkan/drm_scanout_target.h"
#include "backend/drm_kms_vulkan/modifier_format.h"
#include "backend/drm_kms_vulkan/vulkan_backing_store.h"
#include "engine.h"
#include "engine_switches.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "task_runner.h"

namespace {

// Accessor for the dynamic dispatcher (storage in device_caps.cc). A function
// rather than a namespace-scope reference so there is no static-init-order
// dependency on that other TU's global.
const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

// Owns a file descriptor for the rest of its scope.
//
// The scanout barrier hands back an owned sync_file on every explicit-sync
// frame, and each of the present path's exits owes it a close --- including the
// two early returns taken while the presenting layer is still being created.
// drm::sync::SyncFence cannot serve here: import_fd() dups, so handing the fd
// to it leaves the original this side's problem.
class ScopedFd {
 public:
  explicit ScopedFd(const int fd) noexcept : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&&) = delete;
  ScopedFd& operator=(ScopedFd&&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  int fd_{-1};
};

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
// synchronization. Most dependencies of VK_EXT_image_drm_format_modifier
// (bind_memory2, get_memory_requirements2, sampler_ycbcr_conversion,
// get_physical_device_properties2) are core in Vulkan 1.1, which the instance
// targets, so they are not listed. VK_KHR_image_format_list is the exception:
// it went core in 1.2, not 1.1, so under this instance it has to be asked for
// by name. Leaving it out is what validation reports as "Missing extension
// required by the device extension VK_EXT_image_drm_format_modifier" -- the
// device is created anyway and the modifier path appears to work, so nothing
// short of a validation run says otherwise.
constexpr std::array<const char*, 7> kRequiredDeviceExtensions = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
};

// Enabled when the device has it, never required. This array is both the
// device filter and the enable list, so a name in it costs a device that
// lacks it -- and nothing here spends synchronization2: the shell issues
// v1 barriers throughout, and neither Impeller nor Skia references it in
// the engine. It is carried for a plugin, and platform_view.h already tells
// a plugin to fall back when an extension is absent.
constexpr const char* kOptionalDeviceExtensions[] = {
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
    ihs::log::error("[vulkan] {}", msg);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    ihs::log::warn("[vulkan] {}", msg);
  } else {
    ihs::log::debug("[vulkan] {}", msg);
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
// Record @p record into a one-shot command buffer, submit it, and block until
// it retires. The CPU-fence scanout path needs more than a bare barrier in that
// buffer (the platform-view blend goes there too), so the recording is the
// caller's to supply.
template <typename Record>
void SubmitOneShot(
    VkDevice device,
    VkCommandPool pool,
    VkFence fence,
    VkQueue queue,
    Record&& record,
    const std::vector<VkSemaphore>& waits = {},
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) {
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
  record(cmd);
  d().vkEndCommandBuffer(cmd);

  const std::vector<VkPipelineStageFlags> wait_stages(waits.size(), wait_stage);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
  si.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
  si.pWaitDstStageMask = waits.empty() ? nullptr : wait_stages.data();
  d().vkResetFences(device, 1, &fence);
  d().vkQueueSubmit(queue, 1, &si, fence);
  d().vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  d().vkFreeCommandBuffers(device, pool, 1, &cmd);
}

void SubmitImageBarrier(VkDevice device,
                        VkCommandPool pool,
                        VkFence fence,
                        VkQueue queue,
                        const VkImageMemoryBarrier& barrier,
                        VkPipelineStageFlags src_stage,
                        VkPipelineStageFlags dst_stage) {
  SubmitOneShot(device, pool, fence, queue, [&](VkCommandBuffer cmd) {
    d().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
  });
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
  // @p expected_to_fail suppresses the error log for the setup probe, where a
  // refusal is a branch rather than a fault; the caller reports it instead.
  std::optional<size_t> AddSlot(const drm::Device& dev,
                                const drm_kms_vulkan::VulkanBackingStore& store,
                                uint32_t fourcc,
                                bool expected_to_fail = false) {
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
      const std::string detail = fmt::format(
          "[VulkanDrmBackend] framebuffer import ({}x{} fourcc=0x{:08x} "
          "mod=0x{:016x} planes={} offset={} pitch={}): {}",
          store.width(), store.height(), fourcc, mod, planes.size(),
          planes.empty() ? 0u : planes[0].offset,
          planes.empty() ? 0u : planes[0].pitch, src.error().message());
      if (expected_to_fail) {
        ihs::log::debug(detail);
      } else {
        ihs::log::error(detail);
      }
      return std::nullopt;
    }
    slots_.push_back(std::move(src.value()));
    return slots_.size() - 1;
  }

  void SetReady(const size_t slot) noexcept { ready_ = slot; }
  // Explicit-sync variant: stash a render-done fence on the ready slot's
  // source. The scene wires it to the plane's IN_FENCE_FD, so KMS waits on the
  // GPU instead of the raster thread CPU-blocking on the scanout barrier.
  void SetReady(const size_t slot, drm::sync::SyncFence acquire) noexcept {
    ready_ = slot;
    slots_[slot]->set_acquire_fence(std::move(acquire));
  }
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

std::atomic<VulkanDrmBackend*> VulkanDrmBackend::s_active_{nullptr};

VulkanDrmBackend::VulkanDrmBackend(std::string drm_device,
                                   const bool enable_validation,
                                   homescreen::DrmSession* session,
                                   std::string mode_spec,
                                   std::string connector_name,
                                   const int rotation)
    : drm_device_(std::move(drm_device)),
      mode_spec_(std::move(mode_spec)),
      connector_name_(std::move(connector_name)),
      rotation_(rotation),
      enable_validation_(enable_validation),
      session_(session) {
  // Motion-to-photon (IVI_M2P_PROFILE): the flip path feeds RecordPresent and
  // DrmSeat feeds RecordInput, both marshaled onto the platform task runner.
  InitMotionToPhoton();
  s_active_.store(this, std::memory_order_release);
}

VulkanDrmBackend::~VulkanDrmBackend() {
  VulkanDrmBackend* self = this;
  s_active_.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  // Cadence profile summary (no-op unless IVI_PROFILE / IVI_DRMVK_PROFILE ran).
  frame_profile_.LogSessionSummary("VulkanDrmBackend");
  // Anything holding Vulkan objects has to be freed while the device is still
  // alive, because Teardown() destroys it and members outlive this body. Wait
  // for the GPU first: the last frame's command buffer may still reference the
  // render pass and descriptors being freed just below.
  if (device_ != VK_NULL_HANDLE) {
    d().vkDeviceWaitIdle(device_);
  }
#if BUILD_HUD
  // ~VulkanHud runs ImGui_ImplVulkan_Shutdown, which frees device memory.
  hud_.reset();
#endif
#if BUILD_COMPOSITOR
  // ~LayerCompositor destroys its render pass, pipelines, samplers, descriptor
  // pools and cached framebuffers/views. As a member it would otherwise be
  // destroyed after this body -- and so after Teardown() -- calling vkDestroy*
  // on a dead device.
  layer_compositor_.reset();
#endif
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
    const std::string& connector_name,
    const int rotation,
    const drm_config::TriState explicit_sync) {
  auto backend = std::shared_ptr<VulkanDrmBackend>(
      new VulkanDrmBackend(drm_device, enable_validation, session, mode_spec,
                           connector_name, rotation));
  backend->explicit_sync_pref_ = explicit_sync;
  return FinishCreate(std::move(backend));
}

std::shared_ptr<VulkanDrmBackend> VulkanDrmBackend::Create(
    const int drm_fd,
    std::shared_ptr<void> fd_owner,
    const std::string& drm_device,
    const bool enable_validation,
    const std::string& mode_spec,
    const int rotation,
    const uint32_t connector_id,
    std::function<bool()> revoked,
    const drm_config::TriState explicit_sync) {
  if (drm_fd < 0) {
    ihs::log::critical("[VulkanDrmBackend] Create: invalid lease fd {}",
                       drm_fd);
    return nullptr;
  }
  // session is null by construction: on a lease the compositor owns the seat's
  // session, so there is no libseat session to pause/resume against. The
  // existing bring-up already tolerates a null session.
  // No connector name on this tier: the lease already fixed the connector by
  // id, and that outranks anything the operator named.
  auto backend = std::shared_ptr<VulkanDrmBackend>(new VulkanDrmBackend(
      drm_device, enable_validation, /*session=*/nullptr, mode_spec,
      /*connector_name=*/std::string(), rotation));
  backend->injected_fd_ = drm_fd;
  backend->fd_owner_ = std::move(fd_owner);
  // Set BEFORE FinishCreate: SetupCompositor runs inside it and is what pins
  // the connector.
  backend->lease_connector_id_ = connector_id;
  backend->lease_revoked_ = std::move(revoked);
  backend->explicit_sync_pref_ = explicit_sync;
  return FinishCreate(std::move(backend));
}

std::shared_ptr<VulkanDrmBackend> VulkanDrmBackend::FinishCreate(
    std::shared_ptr<VulkanDrmBackend> backend) {
  std::string err;
  if (!backend->BringUp(err)) {
    ihs::log::critical("[VulkanDrmBackend] init failed; refusing to start: {}",
                       err);
    return nullptr;
  }
  if (!backend->SetupCompositor(err)) {
    ihs::log::critical(
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

  ihs::log::info(
      "[VulkanDrmBackend] device='{}' driver='{}' vendor=0x{:04x} "
      "device=0x{:04x} api={}.{}.{}",
      caps_.device_name, caps_.driver_name, caps_.vendor_id, caps_.device_id,
      VK_VERSION_MAJOR(caps_.api_version), VK_VERSION_MINOR(caps_.api_version),
      VK_VERSION_PATCH(caps_.api_version));
  ihs::log::info(
      "[VulkanDrmBackend] caps: drm_node={} timeline_sem={} sync2={} "
      "global_priority={} "
      "lazy_transient={} dedicated_transfer={} gfx_queues={} max_image_2d={}",
      caps_.has_physical_device_drm, caps_.has_timeline_semaphore,
      caps_.has_synchronization2, caps_.has_global_priority,
      caps_.has_lazy_transient, caps_.has_dedicated_transfer_queue,
      caps_.graphics_queue_count, caps_.max_image_2d);
  ihs::log::info(
      "[VulkanDrmBackend] graphics queue family {} created; scanout node '{}'",
      graphics_queue_family_, drm_device_);

  return true;
}

// ── Compositor (present path)
// ─────────────────────────────────────────────────

namespace {

constexpr uint32_t kStageWindow = 60;

// Whether the per-frame heartbeat is compiled in; see its use in the present
// path. Release builds drop it.
#ifdef NDEBUG
constexpr bool kHeartbeat = false;
#else
constexpr bool kHeartbeat = true;
#endif

uint64_t MonotonicNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Where a present's wall time goes, in the same shape drm_kms_egl reports, so
// the two backends can be compared on one panel without converting anything.
// The interval counters next door say how often frames land; this says why.
struct StageProfile {
  uint64_t barrier_sum{0};
  uint64_t wait_sum{0};
  uint64_t commit_sum{0};
  uint64_t total_sum{0};
  uint64_t barrier_max{0};
  uint64_t wait_max{0};
  uint64_t commit_max{0};
  uint64_t total_max{0};
  uint32_t frames{0};

  void account(const uint64_t barrier,
               const uint64_t wait,
               const uint64_t commit,
               const uint64_t total) {
    barrier_sum += barrier;
    wait_sum += wait;
    commit_sum += commit;
    total_sum += total;
    barrier_max = std::max(barrier_max, barrier);
    wait_max = std::max(wait_max, wait);
    commit_max = std::max(commit_max, commit);
    total_max = std::max(total_max, total);
    ++frames;
  }

  void reset() { *this = StageProfile{}; }
};

}  // namespace

struct VulkanDrmBackend::CompositorState {
  explicit CompositorState(drm::Device d) : device(std::move(d)) {}

  // Stop the async flip reader: cancel the pending async_wait, release (not
  // close — the drm::Device owns the fd) and join. Idempotent.
  void StopFlipReader() {
    if (!flip_reader_running) {
      return;
    }
    if (flip_fd.has_value()) {
      std::error_code ec;
      flip_fd->cancel(ec);
      (void)flip_fd->release();
      flip_fd.reset();
    }
    flip_work.reset();
    if (flip_ioc) {
      flip_ioc->stop();
    }
    if (flip_thread.joinable()) {
      flip_thread.join();
    }
    flip_ioc.reset();
    flip_reader_running = false;
  }

  ~CompositorState() {
    // Stop the reader first so nothing else touches the fd while we drain, then
    // drain any lingering flip synchronously so KMS is not mid-page-flip on a
    // buffer we free, then idle the GPU before releasing Vulkan resources.
    StopFlipReader();
    if (flip_pending.load(std::memory_order_acquire)) {
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
    // Explicit-sync ring: fences + semaphores (the command buffers are freed
    // with the pool below). Device is idle here, so nothing is in flight.
    for (VkFence f : sync_fence) {
      if (f != VK_NULL_HANDLE) {
        d().vkDestroyFence(vk_device, f, nullptr);
      }
    }
    for (VkSemaphore s : sync_sem) {
      if (s != VK_NULL_HANDLE) {
        d().vkDestroySemaphore(vk_device, s, nullptr);
      }
    }
    for (const auto& frame_scratch : sample_scratch) {
      for (const SampleScratch& s : frame_scratch) {
        if (s.image != VK_NULL_HANDLE) {
          d().vkDestroyImage(vk_device, s.image, nullptr);
        }
        if (s.memory != VK_NULL_HANDLE) {
          d().vkFreeMemory(vk_device, s.memory, nullptr);
        }
      }
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
  // Set only when the display cannot import what the GPU exports, and the
  // buffers therefore come from a contiguous heap instead. Null means the
  // ordinary exported path; see the scanout probe in SetupCompositor.
  std::unique_ptr<drm_kms_vulkan::ContiguousAllocator> contiguous;
  // Per-stage present timings, when IVI_PROFILE / IVI_DRMVK_PROFILE is on.
  StageProfile stages;
  // Root-surface path only: the slot handed to the engine by get_next_image and
  // presented by present_image. -1 when that path is not in use.
  int root_slot = -1;

  // Ring of scanout buffers. The engine cycles through them
  // (avoid_backing_store_cache), rendering into a free slot while KMS scans
  // another; a single persistent layer presents whichever slot is ready.
  static constexpr size_t kMaxRing = 6;
  struct Slot {
    std::unique_ptr<drm_kms_vulkan::VulkanBackingStore> store;
    bool engine_owned = false;  // handed to the engine, not yet collected
    // Plane path only: this store's own framebuffer, as a scene source.
    // Built on first use and moved into the scene by add_layer, so a null
    // here means either "not on a plane" or "the scene owns it"; the layer
    // the scene keyed on store.get() is the authority.
    std::unique_ptr<VkBackingStoreLayerSource> scene_source;
  };
  std::vector<Slot> slots;
  std::unordered_map<const void*, size_t> key_to_slot;
  // Single-layer path: the engine cycles slots and one persistent layer
  // presents whichever is ready. Unused when every layer gets its own plane.
  std::unique_ptr<VkScanoutRing>
      ring_owner;                 // until moved into the scene layer
  VkScanoutRing* ring = nullptr;  // stable; owned by the scene layer
  std::optional<drm::scene::LayerHandle> layer;
  // Plane path: identity tags the scene currently holds a layer for -- store
  // pointers for backing stores, ICompositorSurface pointers for platform
  // views. drm-cxx retains layers across commits and exposes no iteration
  // beyond find_by_identity_tag, so the prune list lives here.
  std::vector<const void*> plane_layer_keys;
  // Buffers a platform view's pool displaced, held until the next flip
  // completes. drm-cxx fires on_release at displacement, not at flip, so
  // returning one straight away hands the producer a slot KMS is still
  // scanning out.
  struct DeferredRelease {
    std::shared_ptr<ICompositorSurface> surface;
    uint32_t buffer_id = 0;
    drm::sync::SyncFence fence;
  };
  std::mutex deferred_releases_mu;
  std::vector<DeferredRelease> deferred_releases;
  // A layer entered or left the scene, so the next commit must be a blocking
  // modeset: a plane appearing or leaving under NONBLOCK returns EBUSY.
  bool plane_topology_changed = false;
  // Signature of the layer set the last successful test() answered for, so an
  // unchanged frame commits without re-asking.
  size_t plane_plan_sig = 0;
  bool plane_plan_sig_valid = false;

  // Vsync pacing. The first commit is a blocking modeset; subsequent commits
  // are non-blocking page flips whose completion the async reader drains.
  bool first_commit = true;
  // Set on the raster thread when a non-blocking flip is queued, cleared on the
  // reader thread when its event arrives — hence atomic. scanning_slot /
  // pending_slot stay raster-thread-local (present_layers only).
  std::atomic<bool> flip_pending{false};
  int scanning_slot = -1;  // slot the CRTC is currently scanning
  int pending_slot = -1;   // slot in the in-flight non-blocking flip
  // Plane path: the same two facts, but per plane. One plane scans one slot,
  // so the ints above suffice for the blend path; with a plane per layer
  // several slots are live at once and recycling any of them tears the frame
  // KMS is still reading.
  std::vector<size_t> plane_scanning_slots;
  std::vector<size_t> plane_pending_slots;
  uint32_t period_ns = 16'666'667U;  // connector refresh period (from setup)
  drmEventContext evctx{};

  // Async page-flip reader: an asio descriptor over the DRM fd on its own
  // io_context thread. async_wait -> drmHandleEvent(evctx) -> OnFlipEvent,
  // which returns the vsync baton. Replaces the synchronous WaitForFlip poll so
  // the raster thread never blocks and Flutter paces to the real refresh.
  std::unique_ptr<asio::io_context> flip_ioc;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>>
      flip_work;
  std::optional<asio::posix::stream_descriptor> flip_fd;
  std::thread flip_thread;
  bool flip_reader_running = false;

  // Scanout hand-off: drives each backing-store image's layout/cache between
  // the renderer and the KMS scanout engine.
  VkDevice vk_device = VK_NULL_HANDLE;
  VkCommandPool barrier_pool = VK_NULL_HANDLE;
  VkFence barrier_fence = VK_NULL_HANDLE;  // CPU-wait fallback path

  // Explicit-sync ring: the scanout barrier signals sync_sem[i], exported as a
  // sync_file the scene wires to the plane IN_FENCE_FD (KMS waits on the GPU;
  // the raster thread never blocks). N deep so re-recording cmd[i] only waits
  // on the N-frames-old fence, which is already retired — no per-frame stall.
  static constexpr size_t kSyncRing = 3;
  std::array<VkCommandBuffer, kSyncRing> sync_cmd{};
  std::array<VkFence, kSyncRing> sync_fence{};
  std::array<VkSemaphore, kSyncRing> sync_sem{};
  // Producer acquire fences imported for the frame recorded into each ring
  // slot. Destroyed when that slot comes round again, which is after its fence
  // has been waited -- so the submit that waited on them has retired.
  std::array<std::vector<VkSemaphore>, kSyncRing> acquire_waits{};
  // Collected while recording the current frame, moved into the slot at submit.
  std::vector<VkSemaphore> pending_acquire_waits{};

  // Sampleable copies of backing stores the GPU refuses to sample directly
  // (#617). Ringed on the same index as sync_cmd/sync_fence, so the submit
  // that last read scratch[i] is exactly the one waited on before entry i is
  // re-recorded -- which is why the copy needs no synchronization of its own.
  // A vector per frame because one frame can composite several such stores.
  struct SampleScratch {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    VkFormat format{VK_FORMAT_UNDEFINED};
  };
  std::array<std::vector<SampleScratch>, kSyncRing> sample_scratch{};
  bool explicit_sync = false;

  uint64_t frame = 0;
};

bool VulkanDrmBackend::SetupCompositor(std::string& err) {
  drm_kms_vulkan::ScanoutTarget target;
  // Give every layer its own KMS plane rather than blending them into one.
  // Off by default while it proves out; the blend path stays the fallback and
  // is what runs whenever a frame's layers do not all get planes.
  plane_layers_ = std::getenv("IVI_DRMVK_PLANE_LAYERS") != nullptr;
  // An overlay layer needs an alpha channel or it paints opaque black over
  // whatever plane is below it -- Flutter draws transparent pixels where a
  // platform view shows through. The blend path does not care, because it
  // composites into one opaque target, so only ask for alpha when the planes
  // are in play and the choice can still cost a modifier.
  const uint32_t scanout_fourcc =
      plane_layers_ ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
  // Probe through the lease fd when we have one. Not an optimisation: the
  // kernel scopes that fd's view to the leased objects, so this finds exactly
  // the connector we hold, where re-opening the card by path would enumerate
  // the whole card and could pick one the compositor is still driving -- and
  // may not be permitted at all.
  const bool discovered = injected_fd_ >= 0
                              ? drm_kms_vulkan::DiscoverScanoutTarget(
                                    injected_fd_, scanout_fourcc, mode_spec_,
                                    lease_connector_id_, target, err)
                              : drm_kms_vulkan::DiscoverScanoutTarget(
                                    drm_device_, scanout_fourcc, mode_spec_,
                                    connector_name_, target, err);
  if (!discovered) {
    return false;
  }
  // A plane that advertises no IN_FORMATS is not saying "nothing works", it is
  // saying nothing at all: the property is optional and a modifier-blind KMS
  // driver omits it. The legal reading is the implicit modifier, which is
  // linear. Measured on a split render/display SoC where 0 of 14 planes carry
  // IN_FORMATS: gbm allocates LINEAR, drmModeAddFB2WithModifiers accepts a
  // LINEAR framebuffer, and drm-kms-egl scans out on that path at full rate --
  // so refusing here rejected a configuration the same card runs happily.
  //
  // Negotiate against LINEAR rather than against the empty set, so the ICD is
  // still asked whether it can export it. If it cannot, the result is empty
  // and the refusal below stands, which is the honest answer.
  const bool plane_modifiers_unknown = target.plane_modifiers.empty();
  const std::vector<uint64_t> implicit_linear{DRM_FORMAT_MOD_LINEAR};
  const std::vector<uint64_t> allowed = drm_kms_vulkan::NegotiateModifiers(
      physical_device_, VK_FORMAT_B8G8R8A8_UNORM,
      plane_modifiers_unknown ? implicit_linear : target.plane_modifiers);
  if (allowed.empty()) {
    err = plane_modifiers_unknown
              ? "scanout plane advertises no IN_FORMATS and the GPU cannot "
                "export a linear image for this format"
              : "no modifier common to the GPU and the scanout plane";
    return false;
  }
  if (plane_modifiers_unknown) {
    ihs::log::info(
        "[VulkanDrmBackend] scanout plane advertises no IN_FORMATS; using the "
        "implicit (linear) modifier");
  }
  ihs::log::info(
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
    ihs::log::info("[VulkanDrmBackend] plane modifiers advertised:{}", adv);
    ihs::log::info("[VulkanDrmBackend] modifiers negotiated:{}", neg);
  }

  // On a lease, adopt the fd rather than opening the card: it is already master
  // over its leased object set, its view is correctly scoped, and a leased
  // client may not be permitted to open the node at all. from_fd borrows -- the
  // fd stays owned by fd_owner_ (the LeaseHold), so CompositorState's Device
  // will not close it.
  std::optional<drm::Device> dev;
  if (injected_fd_ >= 0) {
    dev.emplace(drm::Device::from_fd(injected_fd_));
  } else {
    auto dev_exp = drm::Device::open(drm_device_);
    if (!dev_exp) {
      err = "drm::Device::open: " + dev_exp.error().message();
      return false;
    }
    dev.emplace(std::move(dev_exp.value()));
  }
  auto state = std::make_unique<CompositorState>(std::move(*dev));
  (void)state->device.enable_atomic();
  (void)state->device.enable_universal_planes();
  state->fourcc = scanout_fourcc;
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
      ihs::log::warn(
          "[VulkanDrmBackend] no tiled non-DCC modifier available for a "
          "{}-degree rotation; the rotated scanout will not commit",
          rotation_);
      state->scanout_modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
    } else {
      std::string picked;
      for (const auto m : state->scanout_modifiers) {
        picked += " " + drm_kms_vulkan::DescribeModifier(m);
      }
      ihs::log::info(
          "[VulkanDrmBackend] rotation {} backing-store modifiers:{}",
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

  if (plane_layers_) {
    // Bias every overlay's zpos by the primary's own. A raw zpos of 1 sits
    // below a primary whose zpos is 2 (amdgpu does this) and inverts the
    // stack; zpos_min is the right read, being the only legal value when the
    // property is immutable and the lowest slot when it is not.
    if (auto reg = drm::planes::PlaneRegistry::enumerate(state->device)) {
      for (const auto& p : reg->all()) {
        if (p.id == target.primary_plane_id) {
          primary_zpos_ = static_cast<int>(p.zpos_min.value_or(0));
          break;
        }
      }
    }
    ihs::log::info(
        "[VulkanDrmBackend] plane layers enabled (IVI_DRMVK_PLANE_LAYERS); "
        "primary zpos {}, backing stores ARGB8888",
        primary_zpos_);
  }

  // Probe the scanout path once: allocate a mode-sized backing store and try to
  // import it as a KMS framebuffer. This is also what chooses between the two
  // allocation directions, and it lets the hardware answer rather than a driver
  // allowlist: a display whose scanout engine has no IOMMU cannot address the
  // render GPU's scattered export, and says so here by failing the import. The
  // fallback below allocates contiguously instead and has both sides import
  // that. Doing it once at setup keeps a per-frame CreateBackingStore from
  // discovering the same thing against a live display. The probe image and its
  // framebuffer are released immediately (ring before store, so the framebuffer
  // is gone before the memory it references).
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
        !probe_ring.AddSlot(state->device, *probe, state->fourcc,
                            /*expected_to_fail=*/true)) {
      // The display cannot address what the GPU exported. Turn the buffer
      // around: allocate it from a contiguous heap instead, and have both the
      // display and the GPU import that. Probed the same way, so the fallback
      // is only adopted once it has actually scanned in.
      probe.reset();
      std::string alloc_err;
      auto contiguous = drm_kms_vulkan::ContiguousAllocator::Open(alloc_err);
      if (!contiguous) {
        err =
            "the display cannot import the GPU's exported buffers and no "
            "contiguous dma-heap is available to allocate from instead (" +
            alloc_err + "); use the GL backend (drm_kms_egl) on this hardware";
        return false;
      }
      std::string imported_err;
      auto imported = drm_kms_vulkan::VulkanBackingStore::CreateImported(
          physical_device_, device_, state->width, state->height,
          VK_FORMAT_B8G8R8A8_UNORM, state->fourcc, *contiguous, imported_err);
      if (!imported) {
        err =
            "the display cannot import the GPU's exported buffers, and the "
            "contiguous fallback failed to allocate: " +
            imported_err;
        return false;
      }
      if (VkScanoutRing imported_ring;
          !imported_ring.AddSlot(state->device, *imported, state->fourcc)) {
        err =
            "neither the GPU's exported buffers nor a contiguous heap "
            "allocation can be scanned out on this display — use the GL "
            "backend (drm_kms_egl) on this hardware";
        return false;
      }
      ihs::log::info(
          "[VulkanDrmBackend] the display cannot import the GPU's exported "
          "buffers; scanning out of the '{}' contiguous heap instead",
          contiguous->name());
      // Linear is the only layout a contiguous heap allocation has, so a
      // rotation that needed a tiled modifier cannot be served this way. 0/180
      // never did, so only the swapping rotations lose anything here.
      if (swap) {
        ihs::log::warn(
            "[VulkanDrmBackend] rotation {} needs a tiled modifier, which the "
            "contiguous scanout path cannot provide; the rotated commit will "
            "fail",
            rotation_);
      }
      state->scanout_modifiers.assign(1, DRM_FORMAT_MOD_LINEAR);
      state->contiguous = std::move(contiguous);
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

  // Explicit-sync ring (VK_KHR_external_semaphore_fd). Build a small ring of
  // (command buffer, signaled fence, SYNC_FD-exportable semaphore) so the
  // scanout barrier signals a fence the kernel waits on via IN_FENCE_FD rather
  // than the raster thread CPU-blocking. If the device can't export a SYNC_FD
  // semaphore, explicit_sync stays false and the CPU-fence path (barrier_fence)
  // runs — same output, one raster-thread stall per frame.
  // --drm-explicit-sync / [view] drm_explicit_sync. kNo takes the CPU-fence
  // path outright; kYes is a demand, and is answered below rather than here so
  // a device that cannot export a SYNC_FD semaphore fails loudly instead of
  // quietly running the fallback the operator just ruled out. The env var stays
  // as the older spelling of kNo.
  const bool sync_forced_off =
      explicit_sync_pref_ == drm_config::TriState::kNo ||
      std::getenv("IVI_DRMVK_NO_EXPLICIT_SYNC") != nullptr;
  if (d().vkGetSemaphoreFdKHR != nullptr && !sync_forced_off) {
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = state->barrier_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = CompositorState::kSyncRing;
    bool ok = d().vkAllocateCommandBuffers(
                  device_, &cbai, state->sync_cmd.data()) == VK_SUCCESS;
    for (size_t i = 0; ok && i < CompositorState::kSyncRing; ++i) {
      // Signaled so the first kSyncRing re-record waits pass immediately.
      VkFenceCreateInfo sfci{};
      sfci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      sfci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      ok = d().vkCreateFence(device_, &sfci, nullptr, &state->sync_fence[i]) ==
           VK_SUCCESS;
      if (!ok) {
        break;
      }
      VkExportSemaphoreCreateInfo esci{};
      esci.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
      esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      VkSemaphoreCreateInfo sci{};
      sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      sci.pNext = &esci;
      ok = d().vkCreateSemaphore(device_, &sci, nullptr, &state->sync_sem[i]) ==
           VK_SUCCESS;
    }
    state->explicit_sync = ok;
  }
  // An explicit demand that the device cannot meet is an error, not a silent
  // downgrade: the operator asked for KMS-side waits, and the CPU-fence path
  // stalls the raster thread every frame instead. Saying so beats leaving them
  // to infer it from a log line they did not know to read.
  if (!state->explicit_sync &&
      explicit_sync_pref_ == drm_config::TriState::kYes) {
    err =
        "--drm-explicit-sync=yes but the device cannot export a SYNC_FD "
        "semaphore (VK_KHR_external_semaphore_fd); pass auto to fall back to "
        "the CPU-fence path";
    return false;
  }
  ihs::log::info("[VulkanDrmBackend] scanout sync: {}{}",
                 state->explicit_sync ? "explicit (IN_FENCE_FD)" : "CPU fence",
                 sync_forced_off ? " (forced off by configuration)" : "");

  // Page-flip event routing: the commit passes `this` as user_data (see
  // present), so the handler recovers the backend and returns the vsync baton
  // with the kernel scanout time.
  state->evctx.version = 2;
  state->evctx.page_flip_handler = [](int /*fd*/, unsigned int /*sequence*/,
                                      unsigned int tv_sec, unsigned int tv_usec,
                                      void* user_data) {
    static_cast<VulkanDrmBackend*>(user_data)->OnFlipEvent(tv_sec, tv_usec);
  };

  if (drmSetMaster(state->device.fd()) != 0) {
    if (const int set_err = errno;
        set_err == EBUSY || set_err == EACCES || set_err == EPERM) {
      // Read-only enumeration (DrmOutputProvider / ResolveDrmDevice) succeeds
      // without master, so a card can resolve here yet refuse master because
      // another DRM master already holds it.
      err = std::string("cannot acquire DRM master (") +
            std::strerror(set_err) +
            "): another display server (gdm / gnome-shell / sddm / Xorg / a "
            "Wayland compositor) already holds this card. Stop it or run from "
            "a bare TTY.";
    } else {
      err = std::string("drmSetMaster: ") + std::strerror(set_err);
    }
    return false;
  }
  state->have_master = true;

  width_ = state->width;
  height_ = state->height;
  compositor_ = std::move(state);
  ihs::log::info(
      "[VulkanDrmBackend] compositor ready: {}x{}, DRM master acquired", width_,
      height_);

  // Refresh-adaptive pacing: tell the vsync provider the connector's period so
  // Flutter targets the real refresh, and start the async page-flip reader that
  // returns each frame's baton on vblank.
  const uint32_t vr = target.mode.vrefresh;
  const uint32_t period_ns =
      vr > 0 ? static_cast<uint32_t>(1'000'000'000ULL / vr) : 16'666'667U;
  compositor_->period_ns = period_ns;
  vsync_.SetPeriodNs(period_ns);
  vsync_.EnableProfile(profiling::FrameProfile::Enabled("IVI_VSYNC_PROFILE"),
                       "DrmVkVsync");
  {
    CompositorState& c = *compositor_;
    c.flip_ioc = std::make_unique<asio::io_context>(ASIO_CONCURRENCY_HINT_1);
    c.flip_work.emplace(asio::make_work_guard(*c.flip_ioc));
    c.flip_fd.emplace(*c.flip_ioc);
    c.flip_fd->assign(c.device.fd());
    ArmFlipRead();
    c.flip_thread = std::thread([&c]() { c.flip_ioc->run(); });
    c.flip_reader_running = true;
    ihs::log::info("[VulkanDrmBackend] vsync: page-flip reader on fd={}",
                   c.device.fd());
  }

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
    ihs::log::info("[VulkanDrmBackend] no HW cursor (disabled or unavailable)");
  }
#endif
  return true;
}

FlutterVulkanImage VulkanDrmBackend::GetNextImageCb(
    void* /*user_data*/,
    const FlutterFrameInfo* frame_info) {
  FlutterVulkanImage img{};
  img.struct_size = sizeof(FlutterVulkanImage);
  img.image = 0;  // FlutterVulkanImageHandle is an opaque uint64, not a pointer
  img.format = VK_FORMAT_B8G8R8A8_UNORM;

  // Only the root-surface path reaches here. Skia with a compositor never asks
  // for a root image; Impeller does, and rejects an empty one outright
  // ("Invalid VkImage given by the embedder"), which fails rasterization.
  VulkanDrmBackend* self = s_active_.load(std::memory_order_acquire);
  if (self == nullptr || !self->compositor_) {
    return img;
  }
  CompositorState& c = *self->compositor_;
  const uint32_t w = frame_info != nullptr && frame_info->size.width > 0
                         ? frame_info->size.width
                         : c.width;
  const uint32_t h = frame_info != nullptr && frame_info->size.height > 0
                         ? frame_info->size.height
                         : c.height;
  const int slot = self->AcquireScanoutSlot(w, h);
  if (slot < 0) {
    return img;
  }
  c.root_slot = slot;
  return *c.slots[static_cast<size_t>(slot)].store->flutter_image();
}

bool VulkanDrmBackend::PresentImageCb(void* /*user_data*/,
                                      const FlutterVulkanImage* /*image*/) {
  VulkanDrmBackend* self = s_active_.load(std::memory_order_acquire);
  if (self == nullptr || !self->compositor_ || !self->compositor_->scene) {
    return false;
  }
  CompositorState& c = *self->compositor_;
  if (c.root_slot < 0) {
    return false;
  }
  const auto slot = static_cast<size_t>(c.root_slot);
  c.root_slot = -1;
  c.slots[slot].engine_owned = false;
  static const bool stage_profile_enabled =
      profiling::FrameProfile::Enabled("IVI_DRMVK_PROFILE");
  const uint64_t t0 = stage_profile_enabled ? MonotonicNs() : 0;
  // No layer stack on this path: the engine rendered the whole frame into the
  // one image, so there is nothing to composite over it.
  return self->PresentSlot(slot, nullptr, 0, t0);
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

// An engine without an Impeller render target for
// kFlutterBackingStoreTypeVulkan logs "Unimplemented", drops the frame, and
// collects the store again. It still calls present_layers, but with no layers
// -- measured: count=0 every frame, against count=1 carrying a Vulkan backing
// store on an engine that can. So it takes store after store and never presents
// one. Say so once, because the alternative is a black panel and no shell-level
// error (flutter/flutter#187525).
void VulkanDrmBackend::ReportIfEngineNeverPresents() {
  // Enough stores that a slow first frame cannot be mistaken for this. The ring
  // is small, so this is reached in well under a second of a stalled engine.
  constexpr uint32_t kStoresBeforeVerdict = 8;
  if (never_presents_reported_ || layers_presented_ ||
      ++backing_stores_created_ < kStoresBeforeVerdict) {
    return;
  }
  never_presents_reported_ = true;
  ihs::log::error(
      "[VulkanDrmBackend] the engine has taken {} backing stores and presented "
      "no layer from any of them. This engine cannot render Impeller into a "
      "Vulkan backing store, so nothing will reach the panel. Drop "
      "--drm-compositor planes to present through the root surface instead "
      "(platform-view layers then do not reach a KMS plane), or run without "
      "--enable-impeller.",
      backing_stores_created_);
}

bool VulkanDrmBackend::CreateBackingStoreImpl(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* out) {
  if (!compositor_) {
    return false;
  }
  ReportIfEngineNeverPresents();
  CompositorState& c = *compositor_;
  const auto w = static_cast<uint32_t>(config->size.width);
  const auto h = static_cast<uint32_t>(config->size.height);

  const int slot = AcquireScanoutSlot(w, h);
  if (slot < 0) {
    return false;
  }
  return FinishBackingStore(slot, out);
}

int VulkanDrmBackend::AcquireScanoutSlot(const uint32_t w, const uint32_t h) {
  CompositorState& c = *compositor_;
  // Reuse a ring slot that the engine has released and the scanout engine is no
  // longer scanning or about to scan; otherwise grow the ring.
  int slot = -1;
  for (size_t i = 0; i < c.slots.size(); ++i) {
    if (const auto& [store, engine_owned, scene_source] = c.slots[i];
        !engine_owned && static_cast<int>(i) != c.scanning_slot &&
        static_cast<int>(i) != c.pending_slot &&
        std::find(c.plane_scanning_slots.begin(), c.plane_scanning_slots.end(),
                  i) == c.plane_scanning_slots.end() &&
        std::find(c.plane_pending_slots.begin(), c.plane_pending_slots.end(),
                  i) == c.plane_pending_slots.end() &&
        store->width() == w && store->height() == h) {
      slot = static_cast<int>(i);
      break;
    }
  }

  if (slot < 0) {
    if (c.slots.size() >= CompositorState::kMaxRing) {
      ihs::log::error(
          "[VulkanDrmBackend] scanout ring exhausted ({} buffers); dropping "
          "frame",
          c.slots.size());
      return -1;
    }
    std::string err;
    // Whichever direction the setup probe settled on. Exported: allocate
    // against the negotiated modifier set (LINEAR when unrotated, a tiled
    // non-DCC modifier for 90/270), the driver picks one and reports its
    // per-plane layout. Imported: one linear buffer from the contiguous heap,
    // whose layout we state. Either way AddSlot imports the framebuffer with
    // store.modifier(), so the layout is honored end to end.
    auto store =
        c.contiguous
            ? drm_kms_vulkan::VulkanBackingStore::CreateImported(
                  physical_device_, device_, w, h, VK_FORMAT_B8G8R8A8_UNORM,
                  c.fourcc, *c.contiguous, err)
            : drm_kms_vulkan::VulkanBackingStore::Create(
                  physical_device_, device_, w, h, VK_FORMAT_B8G8R8A8_UNORM,
                  c.fourcc, c.scanout_modifiers, err);
    if (!store) {
      ihs::log::error("[VulkanDrmBackend] CreateBackingStore({}x{}): {}", w, h,
                      err);
      return -1;
    }
    if (c.ring == nullptr) {
      c.ring_owner = std::make_unique<VkScanoutRing>();
      c.ring = c.ring_owner.get();
    }
    auto idx = c.ring->AddSlot(c.device, *store, c.fourcc);
    if (!idx) {
      ihs::log::error("[VulkanDrmBackend] scanout framebuffer import failed");
      return -1;
    }
    slot = static_cast<int>(*idx);
    c.key_to_slot[store.get()] = static_cast<size_t>(slot);
    c.slots.push_back({std::move(store), false});
    ihs::log::info(
        "[VulkanDrmBackend] scanout ring grew to {} buffer(s) ({}x{})",
        c.slots.size(), w, h);
  }

  return slot;
}

// Hand a slot the engine acquired back to it as a FlutterBackingStore.
bool VulkanDrmBackend::FinishBackingStore(const int slot,
                                          FlutterBackingStore* out) {
  CompositorState& c = *compositor_;
  auto& [store, engine_owned, scene_source] =
      c.slots[static_cast<size_t>(slot)];
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

VsyncCallback VulkanDrmBackend::GetVsyncCallback() const {
  // IVI_DRMVK_VSYNC=0 disables the vsync_callback (wall-clock fallback) —
  // useful to bisect pacing or on hardware where PAGE_FLIP_EVENT delivery is
  // unreliable.
  static const bool enabled = []() {
    const char* env = std::getenv("IVI_DRMVK_VSYNC");
    return !(env != nullptr && env[0] == '0' && env[1] == '\0');
  }();
  return enabled ? &VsyncTrampoline : nullptr;
}

void VulkanDrmBackend::VsyncTrampoline(void* user_data, const intptr_t baton) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return;
  }
  auto* engine_obj = state->view_controller->engine;
  auto* backend = dynamic_cast<VulkanDrmBackend*>(engine_obj->GetBackend());
  if (backend == nullptr) {
    return;
  }
  backend->SetVsyncBaton(engine_obj->GetFlutterEngine(), baton);
}

void VulkanDrmBackend::SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                                     const intptr_t baton) {
  engine_handle_.store(engine, std::memory_order_release);
  // The provider drains the baton inline for an idle pipeline (first frame /
  // wake) or parks it until the in-flight flip returns via DeliverVsync, using
  // our SetSourcePending() state; if the runner isn't wired yet it stays parked
  // and SetEngine's kick latch delivers it.
  vsync_.SubmitBaton(engine, baton);
}

void VulkanDrmBackend::ArmFlipRead() {
  CompositorState* c = compositor_.get();
  if (c == nullptr || !c->flip_fd.has_value()) {
    return;
  }
  c->flip_fd->async_wait(asio::posix::stream_descriptor::wait_read,
                         [this, c](const std::error_code& ec) {
                           if (ec) {
                             return;  // operation_aborted on teardown
                           }
                           // Routes to OnFlipEvent via the commit's user_data.
                           drmHandleEvent(c->device.fd(), &c->evctx);
                           ArmFlipRead();  // re-arm for the next vblank
                         });
}

void VulkanDrmBackend::OnFlipEvent(const unsigned int tv_sec,
                                   const unsigned int tv_usec) {
  CompositorState* c = compositor_.get();
  if (c == nullptr) {
    return;
  }
  c->flip_pending.store(false, std::memory_order_release);
  vsync_.SetSourcePending(false);
  const uint64_t tv_ns = static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL +
                         static_cast<uint64_t>(tv_usec) * 1000ULL;
  // Motion-to-photon scanout endpoint (IVI_M2P_PROFILE). The cutoff is the
  // frame_start of the baton this flip presents; marshaled onto the platform
  // runner so it joins DrmSeat's RecordInput on the same thread.
  profiling::MarshalRecordPresent(
      platform_task_runner_.load(std::memory_order_acquire),
      GetMotionToPhoton(), tv_ns, vsync_.LastDeliveredFrameStartNs(), "drm-vk");
  vsync_.DeliverVsync(tv_ns);  // returns the baton, marshaled onto the runner
}

void VulkanDrmBackend::StopVsyncMonitor() {
  platform_task_runner_.store(nullptr, std::memory_order_release);
  if (compositor_) {
    compositor_->StopFlipReader();
  }
  vsync_.Stop();
  // Motion-to-photon session summary, on the platform thread (the same thread
  // the marshaled RecordInput/RecordPresent tasks ran on).
  if (m2p_enabled_) {
    m2p_.LogSessionSummary("drm-vk");
  }
}

// The layout the engine leaves a backing store in, and the normalization to
// what the scanout hand-off expects.
//
// Skia ends its render in COLOR_ATTACHMENT_OPTIMAL, which is what the barrier
// and the overlay blend below both declare as their source. Impeller ends in
// GENERAL: the embedder render target wraps the image as a swapchain image, and
// Impeller's render pass picks GENERAL as the final layout for those. Declaring
// the wrong old layout does not fail -- the contents simply become undefined --
// which shows as one good frame and then black.
//
// So normalize once, here, rather than teaching every consumer two layouts.
void NormalizeEngineOutputLayout(VkCommandBuffer cmd, VkImage image) {
  if (!ihs::engine_switches::ImpellerActive()) {
    return;
  }
  const VkImageMemoryBarrier barrier = ColorBarrier(
      image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
}

int VulkanDrmBackend::SubmitScanoutBarrier(
    CompositorState& c,
    VkImage image,
    VkImageView view,
    uint32_t width,
    uint32_t height,
    const FlutterLayer** layers,
    size_t count,
    const std::vector<VkImage>* plane_images) {
  if (!c.explicit_sync) {
    // CPU-fence fallback: submit + block until the work retires, then the
    // caller marks the slot ready with no in-fence. (The HUD is folded only
    // into the explicit-sync path, where the scanout fence covers its draw.)
    //
    // The platform-view blend belongs here as much as it does on the explicit
    // path -- it is the same layer stack, and a compositor that drops it shows
    // black views. This branch used to return before reaching it, so any target
    // without IN_FENCE_FD composited the base backing store alone.
    SubmitOneShot(
        device_, c.barrier_pool, c.barrier_fence, graphics_queue_,
        [&](VkCommandBuffer cmd) {
          NormalizeEngineOutputLayout(cmd, image);
          bool composited = false;
#if BUILD_COMPOSITOR
          composited = CompositeOverlays(cmd, layers, count, image, view, width,
                                         height, c.frame);
#endif
          // The blend's render pass ends in GENERAL, so when it runs it is
          // itself the transition and the barrier would repeat it from a layout
          // the image no longer has.
          if (!composited) {
            const VkImageMemoryBarrier barrier = ColorBarrier(
                image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT);
            d().vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                1, &barrier);
          }
        },
        c.pending_acquire_waits);
    // This path blocks until the submit retires, so the imported semaphores
    // cannot outlive it and are freed here rather than against a ring slot.
    for (VkSemaphore sem : c.pending_acquire_waits) {
      d().vkDestroySemaphore(device_, sem, nullptr);
    }
    c.pending_acquire_waits.clear();
    return -1;
  }

  const size_t i = c.frame % CompositorState::kSyncRing;
  // The submit that last used ring entry i was kSyncRing frames ago and is long
  // retired (KMS scanned it out), so this wait never stalls — it only makes the
  // command buffer safe to re-record.
  d().vkWaitForFences(device_, 1, &c.sync_fence[i], VK_TRUE, UINT64_MAX);
  d().vkResetFences(device_, 1, &c.sync_fence[i]);
  // That fence covered the submit which waited on this slot's acquire
  // semaphores, so they are now safe to destroy. A SYNC_FD import is
  // single-use: the wait resets the semaphore to unsignaled, so none can be
  // reused across frames.
  for (VkSemaphore sem : c.acquire_waits[i]) {
    d().vkDestroySemaphore(device_, sem, nullptr);
  }
  c.acquire_waits[i].clear();
  c.pending_acquire_waits.clear();
  VkCommandBuffer cmd = c.sync_cmd[i];
  d().vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d().vkBeginCommandBuffer(cmd, &bi);

  // Barrier the engine's render (COLOR_ATTACHMENT_OPTIMAL) to GENERAL. When the
  // HUD will draw, make its writes ordered against the HUD render pass (a color
  // attachment read/write) instead of just flushing to scanout.
#if BUILD_HUD
  const bool hud_active = hud_enabled_ || !hud_checked_ ||
                          std::getenv("IVI_HUD") != nullptr ||
                          hud_config_enable_;
#else
  const bool hud_active = false;
#endif
  // Plane path: every store reaches scanout on its own plane, so there is
  // nothing to blend and each one only needs its writes made visible to KMS.
  if (plane_images != nullptr) {
    for (VkImage img : *plane_images) {
      NormalizeEngineOutputLayout(cmd, img);
      const VkImageMemoryBarrier pb = ColorBarrier(
          img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_MEMORY_READ_BIT);
      d().vkCmdPipelineBarrier(cmd,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &pb);
    }
    d().vkEndCommandBuffer(cmd);
    return SubmitSyncRing(c, i);
  }
  NormalizeEngineOutputLayout(cmd, image);
#if BUILD_COMPOSITOR
  // Blend the platform views and any Flutter overlay stores over the engine's
  // render, which is already resident in this image. The blend's render pass
  // consumes the image as a color attachment and declares GENERAL as its final
  // layout, so when it runs it *is* the transition and the explicit barrier
  // below would be a redundant second one from the wrong source layout.
  const bool composited = CompositeOverlays(cmd, layers, count, image, view,
                                            width, height, c.frame);
#else
  const bool composited = false;
#endif
  if (!composited) {
    const VkImageMemoryBarrier barrier = ColorBarrier(
        image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        hud_active ? (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                   : VK_ACCESS_MEMORY_READ_BIT);
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             hud_active
                                 ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                 : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

#if BUILD_HUD
  // Draw the HUD into the same command buffer (image is now GENERAL), so the
  // exported scanout fence covers it. Leaves the image in GENERAL.
  RecordHud(cmd, view, width, height, layers, count);
#else
  (void)view;
  (void)width;
  (void)height;
  (void)layers;
  (void)count;
#endif
  d().vkEndCommandBuffer(cmd);
  return SubmitSyncRing(c, i);
}

int VulkanDrmBackend::SubmitSyncRing(CompositorState& c, const size_t i) {
  VkCommandBuffer cmd = c.sync_cmd[i];
  // Producer acquire fences collected while recording: the blend samples those
  // images, so the wait belongs at the fragment stage. Ownership moves to the
  // slot before the submit is built, so pWaitSemaphores points at storage that
  // outlives the call rather than at a vector about to be moved from.
  c.acquire_waits[i] = std::move(c.pending_acquire_waits);
  c.pending_acquire_waits.clear();
  const std::vector<VkSemaphore>& waits = c.acquire_waits[i];
  const std::vector<VkPipelineStageFlags> wait_stages(
      waits.size(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
  si.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
  si.pWaitDstStageMask = waits.empty() ? nullptr : wait_stages.data();
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &c.sync_sem[i];
  if (d().vkQueueSubmit(graphics_queue_, 1, &si, c.sync_fence[i]) !=
      VK_SUCCESS) {
    return -1;
  }
  // Export the just-signaled semaphore as a sync_file. SYNC_FD export transfers
  // the payload out, resetting the semaphore for its next turn in the ring.
  VkSemaphoreGetFdInfoKHR gfi{};
  gfi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
  gfi.semaphore = c.sync_sem[i];
  gfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  int fd = -1;
  if (d().vkGetSemaphoreFdKHR(device_, &gfi, &fd) != VK_SUCCESS) {
    return -1;
  }
#if BUILD_COMPOSITOR
  // The submit just recorded is the one that samples the platform views, so
  // its sync_file is exactly "the compositor is done reading your buffer".
  // Publish it before the caller goes on to commit: a producer that submits
  // concurrently then finds a fence waiting rather than -1.
  PublishReleaseFence(fd);
#endif
  return fd;
}

#if BUILD_COMPOSITOR
void VulkanDrmBackend::PublishReleaseFence(const int sync_fd) {
  if (sync_fd < 0) {
    return;
  }
  for (const std::shared_ptr<ICompositorSurface>& surface : sampled_views_) {
    if (!surface) {
      continue;
    }
    // dup, not sync_fd itself: SetReleaseFenceFd takes ownership of what it is
    // given, and the caller closes sync_fd (and may hand it to IN_FENCE_FD).
    // Same shape as the EGL compositor's OUT_FENCE publish.
    const int fd = ::dup(sync_fd);
    if (fd < 0) {
      // Capture errno before logging, which may clobber it. A producer that
      // gets no fence throttles on its ring depth alone for this frame.
      const int dup_errno = errno;
      ihs::log::warn(
          "[VulkanDrmBackend] dup(scanout fence) failed (errno={}); this view "
          "releases on ring depth alone",
          dup_errno);
      continue;
    }
    surface->SetReleaseFenceFd(fd);  // takes ownership
  }
  sampled_views_.clear();
}
#endif

#if BUILD_HUD
bool VulkanDrmBackend::RecordHud(VkCommandBuffer cmd,
                                 VkImageView view,
                                 uint32_t width,
                                 uint32_t height,
                                 const FlutterLayer** layers,
                                 size_t count) {
  if (!hud_checked_) {
    hud_checked_ = true;
    hud_enabled_ = std::getenv("IVI_HUD") != nullptr || hud_config_enable_;
  }
  if (!hud_enabled_ || hud_init_failed_) {
    return false;
  }
  if (!hud_) {
    std::string err;
    hud_ = ihs::hud::VulkanHud::Create(
        instance_, physical_device_, device_, graphics_queue_family_,
        graphics_queue_, reinterpret_cast<void*>(d().vkGetInstanceProcAddr),
        VK_FORMAT_B8G8R8A8_UNORM, CompositorState::kSyncRing, err);
    if (!hud_) {
      hud_init_failed_ = true;
      ihs::log::warn("[VulkanDrmBackend] HUD unavailable ({})", err);
      return false;
    }
    hud_->SetConfig(hud_config_);
    hud_->SetOpen(true);
    ihs::log::info("[VulkanDrmBackend] debug HUD enabled");
  }

  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                          static_cast<uint64_t>(ts.tv_nsec);
  float dt_s = 1.0f / 60.0f;
  if (hud_last_present_ns_ != 0 && now_ns > hud_last_present_ns_) {
    const uint64_t dt = now_ns - hud_last_present_ns_;
    dt_s = static_cast<float>(dt) / 1e9f;
    hud_interval_ms_[hud_interval_head_] = static_cast<float>(dt) / 1e6f;
    hud_interval_head_ = (hud_interval_head_ + 1) % hud_interval_ms_.size();
    if (hud_interval_count_ < hud_interval_ms_.size()) {
      ++hud_interval_count_;
    }
  }
  hud_last_present_ns_ = now_ns;

  ihs::hud::HudStats stats{};
  if (hud_interval_count_ > 0) {
    float sum = 0.0f;
    float worst = 0.0f;
    for (uint32_t i = 0; i < hud_interval_count_; ++i) {
      sum += hud_interval_ms_[i];
      worst = std::max(worst, hud_interval_ms_[i]);
    }
    stats.frame_ms = sum / static_cast<float>(hud_interval_count_);
    stats.frame_max_ms = worst;
    stats.fps = stats.frame_ms > 0.0f ? 1000.0f / stats.frame_ms : 0.0f;
  }
  stats.dmabuf_present = true;  // DRM Vulkan always scans out the dma-buf slot
  stats.explicit_sync = true;   // this path is explicit-sync only
  const uint32_t refresh_ns = compositor_ ? compositor_->period_ns : 0;
  stats.target_fps =
      refresh_ns > 0 ? 1e9f / static_cast<float>(refresh_ns) : 60.0f;

  std::vector<ihs::hud::HudViewSample> views;
  for (size_t i = 0; layers != nullptr && i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (layer != nullptr &&
        layer->type == kFlutterLayerContentTypePlatformView &&
        layer->platform_view != nullptr) {
      views.push_back({layer->platform_view->identifier,
                       static_cast<uint32_t>(layer->size.width),
                       static_cast<uint32_t>(layer->size.height),
                       /*dmabuf=*/true});
    }
  }

  hud_->Render(cmd, view, width, height, dt_s, stats, views);
  return true;
}
#endif  // BUILD_HUD

#if BUILD_COMPOSITOR
void VulkanDrmBackend::RegisterCompositorSurface(
    const FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  const std::lock_guard<std::mutex> lock(compositor_surfaces_mu_);
  compositor_surfaces_[id] = std::move(surface);
}

void VulkanDrmBackend::UnregisterCompositorSurface(
    const FlutterPlatformViewIdentifier id) {
  const std::lock_guard<std::mutex> lock(compositor_surfaces_mu_);
  compositor_surfaces_.erase(id);
}

void VulkanDrmBackend::ResizeCompositorSurface(
    const FlutterPlatformViewIdentifier id,
    const int32_t width,
    const int32_t height) {
  std::shared_ptr<ICompositorSurface> surface;
  {
    const std::lock_guard<std::mutex> lock(compositor_surfaces_mu_);
    if (const auto it = compositor_surfaces_.find(id);
        it != compositor_surfaces_.end()) {
      surface = it->second;
    }
  }
  // Called without the lock held: OnResize reaches into the plugin, which may
  // call back into the registry.
  if (surface) {
    surface->OnResize(width, height);
  }
}

void VulkanDrmBackend::CollectAcquireWait(CompositorState& c,
                                          ICompositorSurface* surface,
                                          const size_t layer) {
  const int fd = surface->TakeLayerAcquireFenceFd(layer);
  if (fd < 0) {
    return;  // implicit-sync producer: it stalled before submitting
  }
  // A SYNC_FD import is necessarily temporary, and the semaphore is reset to
  // unsignaled once waited -- so it is single-use and retired with the slot.
  if (d().vkImportSemaphoreFdKHR == nullptr) {
    ::close(fd);
    return;
  }
  VkSemaphore sem = VK_NULL_HANDLE;
  VkSemaphoreCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (d().vkCreateSemaphore(device_, &sci, nullptr, &sem) != VK_SUCCESS) {
    ::close(fd);
    return;
  }
  VkImportSemaphoreFdInfoKHR ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
  ii.semaphore = sem;
  ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
  ii.fd = fd;  // consumed by a successful import; still ours on failure
  if (d().vkImportSemaphoreFdKHR(device_, &ii) != VK_SUCCESS) {
    ::close(fd);
    d().vkDestroySemaphore(device_, sem, nullptr);
    return;
  }
  c.pending_acquire_waits.push_back(sem);
}

void VulkanDrmBackend::DropPlaneLayers(CompositorState& c) {
  c.plane_plan_sig_valid = false;
  for (const void* key : c.plane_layer_keys) {
    if (auto* layer = c.scene->find_by_identity_tag(const_cast<void*>(key))) {
      c.scene->remove_layer(layer->handle());
    }
  }
  c.plane_layer_keys.clear();
}

// Hand @p db's planes and acquire fence to @p pool. The fence rides the
// submit so the display engine waits on the producer rather than the CPU;
// import_fd dups, so the caller still owns its fds.
namespace {
void SubmitPvPool(drm::scene::ExternalDmaBufPool* pool,
                  const ICompositorSurface::Dmabuf& db) {
  const uint32_t np = db.plane_count < 4 ? db.plane_count : 4;
  std::array<drm::scene::ExternalPlaneInfo, 4> planes{};
  for (uint32_t p = 0; p < np; ++p) {
    planes[p] =
        drm::scene::ExternalPlaneInfo{db.fd[p], db.offset[p], db.stride[p]};
  }
  std::optional<drm::sync::SyncFence> acquire;
  if (db.acquire_fence_fd >= 0) {
    if (auto f = drm::sync::SyncFence::import_fd(db.acquire_fence_fd); f) {
      acquire = std::move(f.value());
    } else {
      ihs::log::warn(
          "[VulkanDrmBackend] pv acquire-fence import failed ({}); implicit "
          "sync this frame",
          f.error().message());
    }
  }
  pool->submit(
      db.buffer_id,
      drm::span<const drm::scene::ExternalPlaneInfo>(planes.data(), np),
      std::move(acquire));
}
}  // namespace

bool VulkanDrmBackend::ReconcilePlatformViewLayer(
    CompositorState& c,
    const FlutterLayer& fl,
    const int z_index,
    std::vector<const void*>& present) {
  std::shared_ptr<ICompositorSurface> surface;
  {
    const std::lock_guard<std::mutex> lock(compositor_surfaces_mu_);
    if (const auto it = compositor_surfaces_.find(fl.platform_view->identifier);
        it != compositor_surfaces_.end()) {
      surface = it->second;
    }
  }
  if (!surface) {
    return false;
  }
  const void* key = surface.get();
  present.push_back(key);

  surface->OnResize(static_cast<int32_t>(fl.size.width),
                    static_cast<int32_t>(fl.size.height));
  ICompositorSurface::Dmabuf db{};
  const auto state = surface->GetDmabuf(&db);
  // Every populated fd in db is ours now; the pool dups what it keeps.
  const auto close_fds = [&db] {
    for (uint32_t p = 0; p < db.plane_count && p < 4; ++p) {
      if (db.fd[p] >= 0) {
        ::close(db.fd[p]);
      }
    }
    if (db.acquire_fence_fd >= 0) {
      ::close(db.acquire_fence_fd);
    }
  };

  const drm::planes::Rect dst{static_cast<int32_t>(fl.offset.x),
                              static_cast<int32_t>(fl.offset.y),
                              static_cast<uint32_t>(fl.size.width),
                              static_cast<uint32_t>(fl.size.height)};
  const std::optional<int> zpos(primary_zpos_ + z_index);

  if (auto* layer = c.scene->find_by_identity_tag(const_cast<void*>(key))) {
    layer->set_dst_rect_if_changed(dst);
    layer->set_zpos_if_changed(zpos);
    if (state != ICompositorSurface::DmabufState::kFrame) {
      // No new frame this vblank is flow control, not a failure -- the plane
      // keeps scanning what it already has. Anything else (a producer that
      // cannot scan out) has to go back to the blend.
      return state == ICompositorSurface::DmabufState::kNoNewFrame;
    }
    auto* pool =
        dynamic_cast<drm::scene::ExternalDmaBufPool*>(&layer->source());
    if (pool == nullptr) {
      close_fds();
      return false;
    }
    SubmitPvPool(pool, db);
    surface->AckDmabufScanout(db.buffer_id);
    close_fds();
    return true;
  }

  if (state != ICompositorSurface::DmabufState::kFrame) {
    // A view with nothing to show yet: let the blend path handle the frame
    // rather than adding a layer with no buffer.
    return false;
  }
  // The pool caches one fb_id per producer buffer and hands each back through
  // on_release. drm-cxx fires that at displacement rather than at flip, so
  // park it until the next flip completes instead of returning the producer a
  // slot KMS is still scanning.
  drm::scene::ExternalDmaBufPool::Options opts{};
  opts.on_release = [&c, surface](
                        std::uintptr_t rkey,
                        std::optional<drm::sync::SyncFence> release_fence) {
    drm::sync::SyncFence fence;
    if (release_fence.has_value()) {
      fence = std::move(*release_fence);
    }
    const std::scoped_lock lock(c.deferred_releases_mu);
    c.deferred_releases.push_back(
        {surface, static_cast<uint32_t>(rkey), std::move(fence)});
  };
  auto pool_exp = drm::scene::ExternalDmaBufPool::create(
      c.device, db.width, db.height, db.fourcc, db.modifier, std::move(opts));
  if (!pool_exp) {
    ihs::log::debug("[VulkanDrmBackend] pv pool: {}",
                    pool_exp.error().message());
    close_fds();
    return false;
  }
  auto pool = std::move(pool_exp.value());
  auto* pool_raw = pool.get();
  drm::scene::LayerDesc desc{};
  desc.source = std::move(pool);
  desc.display.dst_rect = dst;
  // The producer hands over scanout-oriented (top-down) pixels.
  desc.display.rotation = DRM_MODE_ROTATE_0;
  desc.display.zpos = zpos;
  desc.content_type = drm::planes::ContentType::Generic;
  desc.identity_tag = const_cast<void*>(key);
  auto handle = c.scene->add_layer(std::move(desc));
  if (!handle) {
    ihs::log::debug("[VulkanDrmBackend] pv add_layer: {}",
                    handle.error().message());
    close_fds();
    return false;
  }
  c.plane_layer_keys.push_back(key);
  c.plane_topology_changed = true;
  SubmitPvPool(pool_raw, db);
  surface->AckDmabufScanout(db.buffer_id);
  close_fds();
  return true;
}

bool VulkanDrmBackend::ReconcilePlaneLayers(CompositorState& c,
                                            const FlutterLayer** layers,
                                            const size_t count) {
  // Only backing stores here. A platform view reaching a plane is the same
  // mechanism with a different source (a dma-buf pool fed by the producer),
  // but it is not what makes the blend expensive, so it stays on the blend
  // path and its presence is what forces this frame to bail out below.
  std::vector<const void*> present;
  present.reserve(count);

  int z_index = 0;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* fl = layers[i];
    if (fl == nullptr) {
      continue;
    }
    if (fl->type == kFlutterLayerContentTypePlatformView &&
        fl->platform_view != nullptr) {
      if (!ReconcilePlatformViewLayer(c, *fl, z_index, present)) {
        return false;
      }
      ++z_index;
      continue;
    }
    if (fl->type != kFlutterLayerContentTypeBackingStore ||
        fl->backing_store == nullptr) {
      return false;
    }
    const auto it = c.key_to_slot.find(fl->backing_store->user_data);
    if (it == c.key_to_slot.end()) {
      return false;
    }
    auto& [store, engine_owned, scene_source] = c.slots[it->second];
    if (store == nullptr) {
      return false;
    }
    const void* key = store.get();
    present.push_back(key);

    const drm::planes::Rect dst{static_cast<int32_t>(fl->offset.x),
                                static_cast<int32_t>(fl->offset.y),
                                static_cast<uint32_t>(fl->size.width),
                                static_cast<uint32_t>(fl->size.height)};
    // The bottom layer lands on the primary, whose zpos is immutable on some
    // drivers -- leave it unset and let the allocator place it.
    const std::optional<int> zpos =
        z_index == 0 ? std::nullopt
                     : std::optional<int>(primary_zpos_ + z_index);

    if (auto* layer = c.scene->find_by_identity_tag(const_cast<void*>(key))) {
      layer->set_dst_rect_if_changed(dst);
      layer->set_zpos_if_changed(zpos);
      ++z_index;
      continue;
    }

    if (scene_source == nullptr) {
      std::vector<drm::scene::ExternalPlaneInfo> planes;
      for (const auto& pl : store->planes()) {
        drm::scene::ExternalPlaneInfo info{};
        info.fd = store->dma_buf_fd();
        info.offset = static_cast<uint32_t>(pl.offset);
        info.pitch = static_cast<uint32_t>(pl.pitch);
        planes.push_back(info);
      }
      auto src = VkBackingStoreLayerSource::create(c.device, store->width(),
                                                   store->height(), c.fourcc,
                                                   store->modifier(), planes);
      if (!src) {
        ihs::log::debug("[VulkanDrmBackend] plane layer source: {}",
                        src.error().message());
        return false;
      }
      scene_source = std::move(src.value());
    }

    drm::scene::LayerDesc desc{};
    desc.source = std::move(scene_source);
    desc.display.dst_rect = dst;
    // Vulkan renders top-down, so no REFLECT_Y -- the GL path needs it and
    // pays for it in plane support; this path does not.
    desc.display.rotation = DRM_MODE_ROTATE_0;
    desc.display.zpos = zpos;
    desc.content_type = drm::planes::ContentType::UI;
    desc.identity_tag = const_cast<void*>(key);
    auto handle = c.scene->add_layer(std::move(desc));
    if (!handle) {
      ihs::log::debug("[VulkanDrmBackend] add_layer: {}",
                      handle.error().message());
      return false;
    }
    c.plane_layer_keys.push_back(key);
    ++z_index;
  }

  if (present.empty()) {
    return false;
  }
  // Prune layers for stores that are no longer in the frame, so the scene's
  // allocator is not holding planes for content that left.
  for (auto it = c.plane_layer_keys.begin(); it != c.plane_layer_keys.end();) {
    if (std::find(present.begin(), present.end(), *it) != present.end()) {
      ++it;
      continue;
    }
    if (auto* layer = c.scene->find_by_identity_tag(const_cast<void*>(*it))) {
      c.scene->remove_layer(layer->handle());
    }
    it = c.plane_layer_keys.erase(it);
  }
  return true;
}

VkImage VulkanDrmBackend::AcquireSampleScratch(CompositorState& c,
                                               const size_t slot,
                                               const uint32_t width,
                                               const uint32_t height,
                                               const VkFormat format) {
  auto& frame_scratch = c.sample_scratch[c.frame % CompositorState::kSyncRing];
  if (slot >= frame_scratch.size()) {
    frame_scratch.resize(slot + 1);
  }
  CompositorState::SampleScratch& s = frame_scratch[slot];
  if (s.image != VK_NULL_HANDLE && s.width == width && s.height == height &&
      s.format == format) {
    return s.image;  // reusable: this ring entry's last reader has retired
  }
  // Wrong size or first use. Safe to destroy without a wait for the same
  // reason the copy needs no barrier against other frames: the caller has
  // already waited this ring entry's fence.
  if (s.image != VK_NULL_HANDLE) {
    d().vkDestroyImage(device_, s.image, nullptr);
    s.image = VK_NULL_HANDLE;
  }
  if (s.memory != VK_NULL_HANDLE) {
    d().vkFreeMemory(device_, s.memory, nullptr);
    s.memory = VK_NULL_HANDLE;
  }
  s.width = width;
  s.height = height;
  s.format = format;

  VkImageCreateInfo ic{};
  ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ic.imageType = VK_IMAGE_TYPE_2D;
  ic.format = format;
  ic.extent = {width, height, 1};
  ic.mipLevels = 1;
  ic.arrayLayers = 1;
  ic.samples = VK_SAMPLE_COUNT_1_BIT;
  // Plain OPTIMAL: no modifier, never scanned out, never exported. That is the
  // whole point -- it is free of the constraint that makes the store
  // unsampleable.
  ic.tiling = VK_IMAGE_TILING_OPTIMAL;
  ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (d().vkCreateImage(device_, &ic, nullptr, &s.image) != VK_SUCCESS) {
    s.image = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }

  VkMemoryRequirements req{};
  d().vkGetImageMemoryRequirements(device_, s.image, &req);
  VkPhysicalDeviceMemoryProperties mem{};
  d().vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) != 0 &&
        (mem.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) {
    d().vkDestroyImage(device_, s.image, nullptr);
    s.image = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (d().vkAllocateMemory(device_, &mai, nullptr, &s.memory) != VK_SUCCESS ||
      d().vkBindImageMemory(device_, s.image, s.memory, 0) != VK_SUCCESS) {
    if (s.memory != VK_NULL_HANDLE) {
      d().vkFreeMemory(device_, s.memory, nullptr);
      s.memory = VK_NULL_HANDLE;
    }
    d().vkDestroyImage(device_, s.image, nullptr);
    s.image = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  return s.image;
}

bool VulkanDrmBackend::CompositeOverlays(VkCommandBuffer cmd,
                                         const FlutterLayer** layers,
                                         const size_t count,
                                         VkImage target_image,
                                         VkImageView target_view,
                                         const uint32_t width,
                                         const uint32_t height,
                                         const uint64_t frame) {
  // Cleared before the early return too: a frame that blends nothing must not
  // leave the previous frame's views looking sampled, or PublishReleaseFence
  // would hand them a fence for work that never read them.
  sampled_views_.clear();
  if (layers == nullptr || count < 2) {
    return false;  // base backing store only — nothing to blend over it
  }

  struct Draw {
    VkImage src;  // what is sampled: the store, or a scratch copy of it
    VkFormat format;
    int32_t dx, dy, dw, dh;
    // A Flutter store the engine renders into, to hand back as a color
    // attachment in phase 3, and the layout it is left in here. Not always
    // src: a store that cannot be sampled is copied and the copy is drawn.
    VkImage restore_image;
    VkImageLayout restore_from;
    VkSamplerYcbcrModelConversion ycbcr_model;
    VkSamplerYcbcrRange ycbcr_range;
    // A platform-view layer's crop, placement and orientation, and whether it
    // covers its rect. The defaults draw the whole image, blended.
    UvAffine uv{};
    bool opaque{false};
  };
  std::vector<Draw> draws;
  // Images already moved to SHADER_READ_ONLY this frame, so a buffer shown by
  // two layers is not given a second barrier from a layout it has left.
  std::vector<VkImage> transitioned;
  draws.reserve(count);

  const auto barrier =
      [&](VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
          VkAccessFlags src_access, VkPipelineStageFlags src_stage,
          VkAccessFlags dst_access, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = src_access;
        b.dstAccessMask = dst_access;
        d().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
      };

  CompositorState& c = *compositor_;
  // Scratch copies handed out this frame; one per unsampleable store, so the
  // index is a running count rather than the layer index.
  size_t scratch_used = 0;
  // Phase 1: move every source to SHADER_READ_ONLY. Layout transitions cannot
  // happen inside a render pass, so they all precede phase 2.
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (layer == nullptr) {
      continue;
    }
    const auto dx = static_cast<int32_t>(layer->offset.x);
    const auto dy = static_cast<int32_t>(layer->offset.y);
    const auto dw = static_cast<int32_t>(layer->size.width);
    const auto dh = static_cast<int32_t>(layer->size.height);
    if (dw <= 0 || dh <= 0) {
      continue;
    }

    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store != nullptr) {
      const auto it = c.key_to_slot.find(layer->backing_store->user_data);
      if (it == c.key_to_slot.end()) {
        continue;
      }
      drm_kms_vulkan::VulkanBackingStore* store =
          c.slots[it->second].store.get();
      if (store == nullptr || store->image() == target_image) {
        continue;  // the base store, already resident in the target
      }
      // Where the GPU refused SAMPLED, copy the store into an image that can
      // be sampled and draw that instead (#617). Reached when no modifier the
      // scanout plane accepts also supports sampling, so the store cannot be
      // made sampleable without giving up scanout.
      VkImage sampled = store->image();
      VkImageLayout leaves_store_in = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (!store->sampleable()) {
        VkImage scratch =
            store->copyable()
                ? AcquireSampleScratch(c, scratch_used, store->width(),
                                       store->height(), store->vk_format())
                : VK_NULL_HANDLE;
        if (scratch != VK_NULL_HANDLE) {
          ++scratch_used;
          barrier(store->image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
          // UNDEFINED, not the layout it was left in: every texel is about to
          // be overwritten, so discarding is both legal and cheaper.
          barrier(scratch, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
          VkImageCopy region{};
          region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.extent = {store->width(), store->height(), 1};
          d().vkCmdCopyImage(cmd, store->image(),
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
          barrier(scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
          sampled = scratch;
          leaves_store_in = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else {
          // No copy available either, so the draw below samples an image whose
          // usage never permitted it. Once per process: a property of the
          // GPU's modifier sets, not of a frame.
          static std::once_flag warned_unsampleable;
          std::call_once(warned_unsampleable, [] {
            ihs::log::warn(
                "[VulkanDrmBackend] compositing an overlay layer by sampling a "
                "backing store created without SAMPLED usage, and no "
                "TRANSFER_SRC to copy it with either -- nothing this GPU "
                "scans out can be sampled (#617). Frames may be correct "
                "anyway; the usage is not");
          });
        }
      }
      if (sampled == store->image()) {
        barrier(store->image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      }
      draws.push_back({sampled, store->vk_format(), dx, dy, dw, dh,
                       store->image(), leaves_store_in,
                       VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
                       VK_SAMPLER_YCBCR_RANGE_ITU_NARROW});
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view != nullptr) {
      std::shared_ptr<ICompositorSurface> surface;
      {
        const std::lock_guard<std::mutex> lock(compositor_surfaces_mu_);
        if (const auto it =
                compositor_surfaces_.find(layer->platform_view->identifier);
            it != compositor_surfaces_.end()) {
          surface = it->second;
        }
      }
      if (!surface) {
        continue;
      }
      // Every layer of the view, bottom to top, each cropped, placed within
      // the view and oriented as the producer asked (ihs_pv_submit_layers);
      // a plain producer is one layer drawn whole.
      const RectI view{dx, dy, dw, dh};
      bool sampled = false;
      for (size_t li = 0, n = surface->GetLayerCount(); li < n; ++li) {
        const auto img = surface->GetLayerVulkanImage(li);
        if (img.image == nullptr || img.width <= 0 || img.height <= 0) {
          continue;  // no Vulkan image yet, or a GL-only producer
        }
        const auto& g = img.geometry;
        LayerPlacement place;
        if (!PlaceLayer(g.src, g.dst, g.transform, g.opaque,
                        static_cast<uint32_t>(img.width),
                        static_cast<uint32_t>(img.height), view, &place)) {
          continue;  // entirely outside the view
        }
        auto src = reinterpret_cast<VkImage>(img.image);
        // Wait the producer's work before sampling. An implicit-sync producer
        // stalls before submitting and hands back -1, which is why this path
        // went unexercised; an explicit-sync one hands over a sync_file, and
        // without this the blend samples a buffer the producer may still be
        // writing.
        CollectAcquireWait(c, surface.get(), li);
        // 0 keeps the historical B8G8R8A8_UNORM contract for RGB producers; a
        // planar producer reports its own format plus the conversion
        // parameters.
        const VkFormat src_format = img.format != 0
                                        ? static_cast<VkFormat>(img.format)
                                        : VK_FORMAT_B8G8R8A8_UNORM;
        const auto cur =
            static_cast<VkImageLayout>(surface->GetLayerVulkanImageLayout(li));
        if (cur != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            std::find(transitioned.begin(), transitioned.end(), src) ==
                transitioned.end()) {
          const bool from_preinit = cur == VK_IMAGE_LAYOUT_PREINITIALIZED;
          barrier(src, cur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  from_preinit ? VK_ACCESS_HOST_WRITE_BIT : 0,
                  from_preinit ? VK_PIPELINE_STAGE_HOST_BIT
                               : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
          transitioned.push_back(src);
        }
        surface->SetLayerVulkanImageLayout(
            li, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        draws.push_back(
            {src, src_format, place.dst.x, place.dst.y, place.dst.w,
             place.dst.h, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
             static_cast<VkSamplerYcbcrModelConversion>(img.ycbcr_model),
             static_cast<VkSamplerYcbcrRange>(img.ycbcr_range), place.uv,
             place.opaque});
        sampled = true;
      }
      if (!sampled) {
        continue;
      }
      // This frame's submit reads the view's image, so its completion is what
      // frees the producer's ring slot. Keep the surface alive until that
      // fence has been handed over (the map entry can be erased meanwhile).
      sampled_views_.push_back(std::move(surface));
    }
  }

  if (draws.empty()) {
    return false;
  }

  // Built here rather than at init: the pipeline is keyed to the slot format,
  // and a failure must not be retried every frame.
  if (!layer_compositor_ && !layer_compositor_failed_) {
    std::string err;
    layer_compositor_ = wl_vulkan::LayerCompositor::Create(
        device_, VK_FORMAT_B8G8R8A8_UNORM, err,
        wl_vulkan::LayerCompositor::ContentMode::kPreserve);
    if (!layer_compositor_) {
      layer_compositor_failed_ = true;
      ihs::log::error(
          "[VulkanDrmBackend] platform-view blend unavailable ({}); views will "
          "not composite",
          err);
    }
  }
  if (!layer_compositor_) {
    return false;
  }

  // Phase 2: one render pass, layers blended in z-order over the base.
  bool opened = false;
  if (layer_compositor_->BeginFrame(cmd, target_view, width, height, frame)) {
    opened = true;
    for (const Draw& dr : draws) {
      layer_compositor_->DrawLayer(cmd, dr.src, dr.format, dr.ycbcr_model,
                                   dr.ycbcr_range, dr.dx, dr.dy, dr.dw, dr.dh,
                                   dr.uv, dr.opaque);
    }
    wl_vulkan::LayerCompositor::EndFrame(cmd);
  }

  // Phase 3: hand the Flutter stores back as color attachments for the engine's
  // next render. Runs even if the pass never opened, since phase 1 moved them.
  for (const Draw& dr : draws) {
    if (dr.restore_image == VK_NULL_HANDLE) {
      continue;
    }
    // A copied store was left in TRANSFER_SRC by the copy, not read by the
    // draw; anything else was sampled directly. Restoring from the wrong
    // layout would discard the engine's contents.
    const bool copied = dr.restore_from == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier(dr.restore_image, dr.restore_from,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            copied ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT,
            copied ? VK_PIPELINE_STAGE_TRANSFER_BIT
                   : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  }
  return opened;
}
#endif  // BUILD_COMPOSITOR

bool VulkanDrmBackend::PresentLayersImpl(const FlutterLayer** layers,
                                         size_t count) {
  // A layer here is the evidence that the engine could build a render target
  // from the backing store this backend handed it. An engine that could not
  // still presents -- with no layers at all -- so the test is what arrived,
  // not that present was called. See ReportIfEngineNeverPresents.
  if (count > 0) {
    layers_presented_ = true;
  }
  if (!compositor_ || !compositor_->scene) {
    return false;
  }
  // Lease revoked: the leased KMS objects are gone from this fd's view, so
  // every atomic commit naming them fails -- and a failed commit produces no
  // PAGE_FLIP_EVENT, so the flip path would churn rather than settle. Park the
  // vsync source instead: SubmitBaton parks the baton, no OnVsync fires, and
  // raster stalls, which is the same steady state the libseat pause path
  // produces for the gated state. Returning true
  // reports the frame as presented -- it was not, but the alternative is the
  // engine treating a revoked lease as a render error. Reacquire unparks by
  // reacquiring; until then the exit policy is the way out.
  if (lease_revoked_ && lease_revoked_()) {
    vsync_.SetSourcePending(true);
    if (!lease_revoked_logged_.exchange(true, std::memory_order_relaxed)) {
      ihs::log::warn(
          "[VulkanDrmBackend] lease revoked — gating commits and parking "
          "vsync; the panel stops updating until the lease is reacquired");
    }
    return true;
  }
  CompositorState& c = *compositor_;
  static const bool stage_profile_enabled =
      profiling::FrameProfile::Enabled("IVI_DRMVK_PROFILE");
  const uint64_t t0 = stage_profile_enabled ? MonotonicNs() : 0;
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
  // Try to give every layer its own plane first. Falling through costs a
  // reconcile and a TEST_ONLY commit, which is why the rejection latches a log
  // line rather than being silent -- a frame shape that never places is worth
  // knowing about.
  if (plane_layers_ && !plane_layers_latched_off_ &&
      PresentLayersViaPlanes(layers, count)) {
    return true;
  }
  const auto it = c.key_to_slot.find(bs_layer->backing_store->user_data);
  if (it == c.key_to_slot.end()) {
    return false;
  }
  return PresentSlot(it->second, layers, count, t0);
}

// The present sequence, shared by the two ways a frame arrives: the compositor
// path (present_layers, with a layer stack to composite) and the root-surface
// path (present_image, one image and no layers). Everything from the scanout
// barrier onward is identical; only how the slot was chosen differs.
void VulkanDrmBackend::DrainDeferredScanoutReleases(CompositorState& c) {
  // Take the batch under the lock and fire the callbacks without it: they run
  // producer code (an eventfd signal) and must not re-enter under our mutex.
  std::vector<CompositorState::DeferredRelease> batch;
  {
    const std::scoped_lock lock(c.deferred_releases_mu);
    batch.swap(c.deferred_releases);
  }
  for (auto& r : batch) {
    if (!r.surface) {
      continue;
    }
    // Publish the fence before the release, so a producer woken by the
    // release already has something to wait on rather than racing to read a
    // field about to be written. dup, not fd(): SetReleaseFenceFd takes
    // ownership while SyncFence closes its own when the batch goes out of
    // scope, and handing fd() over directly would double-close.
    if (r.fence.valid()) {
      if (const int fd = ::dup(r.fence.fd()); fd >= 0) {
        r.surface->SetReleaseFenceFd(fd);
      }
    }
    r.surface->OnScanoutRelease(r.buffer_id);
  }
}

bool VulkanDrmBackend::PresentLayersViaPlanes(const FlutterLayer** layers,
                                              const size_t count) {
  CompositorState& c = *compositor_;
  // Buffers displaced by the previous present. drm-cxx releases at
  // displacement, not at flip, so this is deliberately one present late: the
  // flip that retired them is the one waited on just below.
  DrainDeferredScanoutReleases(c);
  if (!ReconcilePlaneLayers(c, layers, count)) {
    DropPlaneLayers(c);
    return false;
  }

  // Ask before recording. A layer the allocator cannot place comes back
  // Composited -- drm-cxx's CPU canvas, far slower than the blend we already
  // have -- or Unassigned, which drops it. Either way the frame goes back to
  // the blend, and that has to be decided now: the command buffer below is
  // recorded on the assumption that nothing needs compositing.
  //
  // All-or-nothing, not per layer. A layer blended into the bottom store sits
  // below anything on an overlay plane, so mixing the two inverts the stack
  // wherever a blended layer belongs above a placed one.
  // Steady state re-asks a question whose inputs have not changed. The
  // allocator is deterministic for a given layer set, so cache the verdict
  // against a signature of it and skip the ioctl while that holds; a topology
  // change or any geometry move invalidates it.
  size_t sig = c.plane_layer_keys.size();
  for (const void* k : c.plane_layer_keys) {
    sig = sig * 1000003U ^ reinterpret_cast<uintptr_t>(k);
  }
  for (size_t li = 0; li < count; ++li) {
    if (layers[li] == nullptr) {
      continue;
    }
    sig = sig * 1000003U ^ static_cast<size_t>(layers[li]->offset.x);
    sig = sig * 1000003U ^ static_cast<size_t>(layers[li]->offset.y);
    sig = sig * 1000003U ^ static_cast<size_t>(layers[li]->size.width);
    sig = sig * 1000003U ^ static_cast<size_t>(layers[li]->size.height);
  }
  if (!c.plane_topology_changed && c.plane_plan_sig_valid &&
      c.plane_plan_sig == sig) {
    return CommitPlaneFrame(c, layers, count, /*assigned=*/0);
  }

  auto test = c.scene->test();
  if (!test) {
    if (test.error() != std::errc::permission_denied) {
      DropPlaneLayers(c);
    }
    return false;
  }
  for (const auto& p : test->placements) {
    if (p.placement != drm::scene::LayerPlacement::AssignedToPlane) {
      if (!plane_test_rejected_) {
        plane_test_rejected_ = true;
        ihs::log::info(
            "[VulkanDrmBackend] {} of {} layers would not get a plane; "
            "blending instead (said once)",
            test->layers_total - test->layers_assigned, test->layers_total);
      }
      DropPlaneLayers(c);
      return false;
    }
  }

  c.plane_plan_sig = sig;
  c.plane_plan_sig_valid = true;
  return CommitPlaneFrame(c, layers, count, test->layers_assigned);
}

bool VulkanDrmBackend::CommitPlaneFrame(CompositorState& c,
                                        const FlutterLayer** layers,
                                        const size_t count,
                                        const size_t assigned) {
  // Every store is scanned out and none is sampled, so the barrier only makes
  // the engine's writes visible: no blend, no copy.
  std::vector<VkImage> images;
  std::vector<size_t> slots;
  images.reserve(count);
  slots.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (layers[i] == nullptr ||
        layers[i]->type != kFlutterLayerContentTypeBackingStore ||
        layers[i]->backing_store == nullptr) {
      continue;
    }
    const auto it = c.key_to_slot.find(layers[i]->backing_store->user_data);
    if (it == c.key_to_slot.end()) {
      continue;
    }
    if (auto* store = c.slots[it->second].store.get(); store != nullptr) {
      images.push_back(store->image());
      slots.push_back(it->second);
    }
  }
  const ScopedFd scanout_fence(SubmitScanoutBarrier(
      c, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0, layers, count, &images));

  // Same single-flip pacing as the blend path: one flip in flight whatever the
  // plane count.
  for (int spin_ms = 0;
       spin_ms < 100 && c.flip_pending.load(std::memory_order_acquire);
       ++spin_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // That flip completed, so the slots it carried are the ones now scanning and
  // whatever they displaced is free to be handed out again.
  if (!c.plane_pending_slots.empty()) {
    c.plane_scanning_slots = std::move(c.plane_pending_slots);
    c.plane_pending_slots.clear();
  }

  // Hand the render-done fence to each store's source so KMS waits on the GPU
  // rather than the raster thread. import_fd dups; ours is ScopedFd's.
  if (scanout_fence.get() >= 0) {
    for (const size_t slot : slots) {
      const auto* store = c.slots[slot].store.get();
      auto* layer = c.scene->find_by_identity_tag(
          const_cast<void*>(static_cast<const void*>(store)));
      if (layer == nullptr) {
        continue;
      }
      if (auto* src =
              dynamic_cast<VkBackingStoreLayerSource*>(&layer->source())) {
        if (auto f = drm::sync::SyncFence::import_fd(scanout_fence.get())) {
          src->set_acquire_fence(std::move(f.value()));
        }
      }
    }
  }

  // A plane entering or leaving needs ALLOW_MODESET, which cannot be
  // non-blocking; steady state is a non-blocking flip we pace against.
  const bool modeset = c.first_commit || c.plane_topology_changed;
  const uint32_t flags =
      modeset ? 0U : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);
  if (auto report = c.scene->commit(flags, this); !report) {
    ihs::log::error("[VulkanDrmBackend] plane commit: {}",
                    report.error().message());
    DropPlaneLayers(c);
    plane_layers_latched_off_ = true;
    ihs::log::warn(
        "[VulkanDrmBackend] plane layers off for this session after a commit "
        "failure; blending from here");
    return false;
  }
  // Say so once, positively. Everything else about this path is visible only
  // as an absence -- no rejection, no fallback warning -- and an absence is
  // not evidence that it ran.
  if (assigned != 0 && plane_layers_confirmed_ != assigned) {
    plane_layers_confirmed_ = assigned;
    ihs::log::info(
        "[VulkanDrmBackend] presenting on KMS planes: {} layers, no blend",
        assigned);
  }
  c.plane_topology_changed = false;
  if (modeset) {
    // Blocking modeset: no flip event, so these slots are already scanning.
    c.first_commit = false;
    c.plane_scanning_slots = slots;
  } else {
    c.plane_pending_slots = slots;
    c.flip_pending.store(true, std::memory_order_release);
    vsync_.SetSourcePending(true);
  }

  // Report each view's plane back, for ihs_pv_grant_drm_plane_id.
  {
    const std::lock_guard<std::mutex> lock(compositor_surfaces_mu_);
    for (auto& [id, surface] : compositor_surfaces_) {
      if (!surface) {
        continue;
      }
      if (auto* layer = c.scene->find_by_identity_tag(
              const_cast<void*>(static_cast<const void*>(surface.get())))) {
        surface->SetScanoutPlane(layer->last_assigned_plane_id().value_or(0));
      }
    }
  }
  ++c.frame;
  return true;
}

bool VulkanDrmBackend::PresentSlot(const size_t slot,
                                   const FlutterLayer** layers,
                                   const size_t count,
                                   const uint64_t t0) {
  CompositorState& c = *compositor_;
  static const bool stage_profile_enabled =
      profiling::FrameProfile::Enabled("IVI_DRMVK_PROFILE");
  drm_kms_vulkan::VulkanBackingStore* store = c.slots[slot].store.get();

  // Flush the renderer's color writes so the scanout engine sees this frame.
  // The embedder host-syncs all layers before present_layers, so the render is
  // already retired; this barrier only makes the writes visible to KMS. On the
  // explicit-sync path it returns a sync_file the scene hands to IN_FENCE_FD so
  // the kernel — not the raster thread — waits on it; -1 means CPU-fenced.
  // Owned here so every exit below closes it exactly once --- the early returns
  // during layer creation included.
  const ScopedFd scanout_fence(
      SubmitScanoutBarrier(c, store->image(), store->view(), store->width(),
                           store->height(), layers, count));
  const uint64_t t1 = stage_profile_enabled ? MonotonicNs() : 0;

  // The engine's raster thread pipelines: it hands us frame N+1 while flip N is
  // still in flight, and a single plane can hold only one flip, so we wait for
  // the previous flip here. Do NOT read the fd — the async reader is the single
  // reader (a second drmHandleEvent would race it); poll the flag instead.
  // A short wait (~1 vblank slice) is the normal cost of single-plane double
  // buffering; only a wait past the refresh period is a real stall (a dropped
  // flip event or buffer starvation), which the counter flags.
  int spin_ms = 0;
  for (; spin_ms < 100 && c.flip_pending.load(std::memory_order_acquire);
       ++spin_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const uint64_t t2 = stage_profile_enabled ? MonotonicNs() : 0;
  const bool stalled =
      static_cast<uint32_t>(spin_ms) * 1'000'000U > c.period_ns;
  // Rotate: the completed flip's slot is now scanning; the slot it replaced
  // returns to the free pool. Raster-thread-local (the reader never touches
  // it).
  if (c.pending_slot >= 0) {
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
      ihs::log::error("[VulkanDrmBackend] add_layer: {}",
                      layer.error().message());
      return false;
    }
    c.layer = layer.value();
  }

  // Mark the slot ready. On the explicit-sync path, hand its render-done
  // sync_file to the source as the acquire fence (the scene lowers it to the
  // plane's IN_FENCE_FD). import_fd dups, so the fence the ring receives is its
  // own; ours is closed by ScopedFd on the way out either way.
  if (scanout_fence.get() >= 0) {
    if (auto fence = drm::sync::SyncFence::import_fd(scanout_fence.get())) {
      c.ring->SetReady(slot, std::move(fence.value()));
    } else {
      c.ring->SetReady(slot);
    }
  } else {
    c.ring->SetReady(slot);
  }
  // First commit is a blocking modeset (the scene adds ALLOW_MODESET, which
  // cannot be non-blocking); subsequent frames are non-blocking page flips we
  // pace against.
  const uint32_t flags =
      c.first_commit ? 0U
                     : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);
  // Pass `this` as the flip user_data so the event routes to OnFlipEvent.
  if (auto report = c.scene->commit(flags, this); !report) {
    ihs::log::error("[VulkanDrmBackend] commit: {}", report.error().message());
    return false;
  }
  if (c.first_commit) {
    // Blocking modeset: no flip event. Leave the source not-pending so the
    // provider drains the next baton inline and the async loop starts.
    c.first_commit = false;
    c.scanning_slot = static_cast<int>(slot);
  } else {
    // Non-blocking flip: mark the source pending so the provider holds the next
    // baton until OnFlipEvent returns it on vblank.
    c.pending_slot = static_cast<int>(slot);
    c.flip_pending.store(true, std::memory_order_release);
    vsync_.SetSourcePending(true);
  }

  const uint64_t n = c.frame++;
  // The first frames at info confirm the pipeline started; the periodic one
  // after that is a heartbeat, and a heartbeat at info never stops -- every two
  // seconds for the life of the process, which is tens of thousands of lines a
  // day in a log an operator is meant to read.
  if (n < 3) {
    ihs::log::info(
        "[VulkanDrmBackend] presented frame {} slot {} ({}x{}); ring={}", n,
        slot, store->width(), store->height(), c.slots.size());
  } else if (kHeartbeat && n % 120 == 0) {
    // Debug records are emitted by default, so a heartbeat left on in a release
    // build costs a line every two seconds for the life of the process. Useful
    // while developing, not in the field.
    ihs::log::debug(
        "[VulkanDrmBackend] presented frame {} slot {} ({}x{}); ring={}", n,
        slot, store->width(), store->height(), c.slots.size());
  }

  if (stage_profile_enabled) {
    frame_profile_.Record("VulkanDrmBackend", /*ok=*/true, /*now_ns=*/0,
                          stalled);
    const uint64_t t3 = MonotonicNs();
    c.stages.account(t1 - t0, t2 - t1, t3 - t2, t3 - t0);
    if (c.stages.frames >= kStageWindow) {
      const auto& st = c.stages;
      const auto mean_ms = [](const uint64_t sum, const uint32_t n) {
        return static_cast<double>(sum) / static_cast<double>(n) / 1e6;
      };
      const auto max_ms = [](const uint64_t v) {
        return static_cast<double>(v) / 1e6;
      };
      ihs::log::info(
          "[VulkanDrmBackend] stage profile (n={}): barrier={:.2f}ms (max "
          "{:.2f})  wait={:.2f}ms (max {:.2f})  commit={:.2f}ms (max {:.2f})  "
          "total={:.2f}ms (max {:.2f})",
          st.frames, mean_ms(st.barrier_sum, st.frames), max_ms(st.barrier_max),
          mean_ms(st.wait_sum, st.frames), max_ms(st.wait_max),
          mean_ms(st.commit_sum, st.frames), max_ms(st.commit_max),
          mean_ms(st.total_sum, st.frames), max_ms(st.total_max));
      c.stages.reset();
    }
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
      ihs::log::warn(
          "[VulkanDrmBackend] validation requested (-d) but "
          "VK_LAYER_KHRONOS_validation is not enumerable; continuing without "
          "validation");
    }
  }

  // Impeller's instance capability check wants VK_KHR_surface plus one entry
  // from a fixed WSI list, and its device check wants VK_KHR_swapchain. None
  // are used -- this backend presents to KMS and creates no VkSurfaceKHR -- but
  // the check reads the list the embedder declares, so declare them. Prefer the
  // candidate that implies no window system; VK_KHR_display is deliberately not
  // among them, as enabling it makes the loader take the display and the
  // backend's own drmSetMaster then fails with EPERM.
  if (ihs::engine_switches::ImpellerActive()) {
    uint32_t avail_n = 0;
    d().vkEnumerateInstanceExtensionProperties(nullptr, &avail_n, nullptr);
    std::vector<VkExtensionProperties> avail(avail_n);
    if (avail_n > 0) {
      d().vkEnumerateInstanceExtensionProperties(nullptr, &avail_n,
                                                 avail.data());
    }
    if (HasExt(avail, "VK_KHR_surface")) {
      enabled_instance_extensions_.push_back("VK_KHR_surface");
      for (const char* wsi :
           {"VK_KHR_portability_enumeration", "VK_KHR_wayland_surface",
            "VK_KHR_xcb_surface", "VK_KHR_xlib_surface"}) {
        if (HasExt(avail, wsi)) {
          enabled_instance_extensions_.push_back(wsi);
          break;
        }
      }
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
  // Also probe pipelineCreationCacheControl: a platform-view engine that adopts
  // this device (e.g. the Mapbox Maps SDK) creates its pipeline cache with
  // EXTERNALLY_SYNCHRONIZED, which needs this feature to be valid.
  VkPhysicalDevicePipelineCreationCacheControlFeatures
      cache_control_supported{};
  cache_control_supported.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
  timeline_supported.pNext = &cache_control_supported;
  VkPhysicalDeviceSynchronization2Features sync2_supported{};
  sync2_supported.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2_supported.pNext = &timeline_supported;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &sync2_supported;
  d().vkGetPhysicalDeviceFeatures2(physical_device_, &features2);

  // Not a refusal. The device is usable without it -- see
  // kOptionalDeviceExtensions -- so record it and enable it only if present,
  // the same way timelineSemaphore below has always been treated.
  caps_.has_synchronization2 = sync2_supported.synchronization2 == VK_TRUE;

  VkPhysicalDeviceSynchronization2Features sync2_enable{};
  sync2_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2_enable.synchronization2 = VK_TRUE;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_enable{};
  timeline_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  timeline_enable.timelineSemaphore = VK_TRUE;
  // Chain head depends on what the device has: without synchronization2 the
  // struct must not be chained at all, or vkCreateDevice is asked to enable a
  // feature from an extension that was never enabled.
  const void* features_chain = nullptr;
  if (timeline_supported.timelineSemaphore == VK_TRUE) {
    features_chain = &timeline_enable;
  }
  if (caps_.has_synchronization2) {
    sync2_enable.pNext = const_cast<void*>(features_chain);
    features_chain = &sync2_enable;
  }

  constexpr float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = graphics_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures device_features{};
  // A platform-view engine that adopts this device creates anisotropic samplers
  // and uses dual-source blending; enable those when the device offers them so
  // such an engine renders validation-clean. Harmless to the compositor.
  device_features.samplerAnisotropy = features2.features.samplerAnisotropy;
  device_features.dualSrcBlend = features2.features.dualSrcBlend;

  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.pNext = features_chain;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  // Required by Impeller's device check; unused on this presentation path.
  if (ihs::engine_switches::ImpellerActive()) {
    uint32_t dev_ext_n = 0;
    d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                             &dev_ext_n, nullptr);
    std::vector<VkExtensionProperties> dev_exts(dev_ext_n);
    if (dev_ext_n > 0) {
      d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                               &dev_ext_n, dev_exts.data());
    }
    if (HasExt(dev_exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
      enabled_device_extensions_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
  }

  // The optional set, enabled only where present. A plugin reads these back
  // through IhsVulkanContext::device_extensions and gates on what it finds, so
  // leaving one out where the device lacks it is the contract working, not a
  // capability lost.
  {
    uint32_t opt_n = 0;
    d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &opt_n,
                                             nullptr);
    std::vector<VkExtensionProperties> opt_exts(opt_n);
    if (opt_n > 0) {
      d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                               &opt_n, opt_exts.data());
    }
    for (const char* opt : kOptionalDeviceExtensions) {
      if (HasExt(opt_exts, opt) &&
          std::find_if(enabled_device_extensions_.begin(),
                       enabled_device_extensions_.end(), [opt](const char* e) {
                         return std::strcmp(e, opt) == 0;
                       }) == enabled_device_extensions_.end()) {
        enabled_device_extensions_.push_back(opt);
      }
    }
  }
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();
  device_info.pEnabledFeatures = &device_features;

  // Enable pipelineCreationCacheControl (and its extension) when the device
  // offers both, so an adopting engine's externally-synchronized pipeline cache
  // is valid. Declared at function scope so it outlives vkCreateDevice.
  VkPhysicalDevicePipelineCreationCacheControlFeatures cache_control_enable{};
  cache_control_enable.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
  if (cache_control_supported.pipelineCreationCacheControl == VK_TRUE) {
    uint32_t ext_count = 0;
    d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                             &ext_count, nullptr);
    std::vector<VkExtensionProperties> avail(ext_count);
    if (ext_count > 0) {
      d().vkEnumerateDeviceExtensionProperties(physical_device_, nullptr,
                                               &ext_count, avail.data());
    }
    if (HasExt(avail, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME)) {
      enabled_device_extensions_.push_back(
          VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
      // push_back may reallocate, so refresh the pointer/count on device_info.
      device_info.enabledExtensionCount =
          static_cast<uint32_t>(enabled_device_extensions_.size());
      device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();
      cache_control_enable.pipelineCreationCacheControl = VK_TRUE;
      cache_control_enable.pNext = const_cast<void*>(device_info.pNext);
      device_info.pNext = &cache_control_enable;
    }
  }

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

bool VulkanDrmBackend::GetVulkanContext(BackendVulkanContext* out) const {
  if (out == nullptr || device_ == VK_NULL_HANDLE) {
    return false;
  }
  out->instance = instance_;
  out->physical_device = physical_device_;
  out->device = device_;
  out->queue = graphics_queue_;
  out->queue_family_index = graphics_queue_family_;
  // KNOWN CONTRACT GAP (issue #208): this hands back the raw loader, but
  // ihs/platform_view.h requires get_instance_proc_addr to be an INTERPOSED
  // loader that serializes vkQueueSubmit/vkQueuePresentKHR on the shared
  // graphics queue (as WaylandVulkanBackend does via QueueInterposer). This
  // backend has no queue mutex yet, so a plugin resolving through this loader
  // would submit unsynchronized against the compositor/engine — undefined
  // behavior per the Vulkan external-synchronization rules. Latent today (no
  // Vulkan-on-DRM plugin is wired to this ABI), but it MUST be interposed
  // before one is. Fixing it means adding a queue mutex, wrapping this
  // backend's own submit/present sites in it, and returning the interposed
  // loader here and to the engine — tracked under #208.
  out->get_instance_proc_addr =
      reinterpret_cast<void*>(d().vkGetInstanceProcAddr);
  out->device_extensions = enabled_device_extensions_.data();
  out->device_extension_count = enabled_device_extensions_.size();
  return true;
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
  // Engines up to and including 3.47 cannot turn a Vulkan backing store into
  // an Impeller render target: embedder.cc logs "Unimplemented" for
  // kFlutterBackingStoreTypeVulkan and rasterization fails outright, leaving a
  // dead panel (flutter/flutter#187525). The engine exposes no way to ask, so
  // under Impeller the compositor is opt-in: --drm-compositor planes selects it
  // for an engine that has the render target, and anything else presents
  // through the root surface -- get_next_image / present_image, which this
  // backend implements. Platform views do not reach a KMS plane on that path,
  // so it is a real reduction, logged rather than silent.
  const bool impeller = ihs::engine_switches::ImpellerActive();
  if (compositor_mode_ == drm_config::Compositor::kGl) {
    ihs::log::info(
        "[VulkanDrmBackend] --drm-compositor gl: presenting through the root "
        "surface; platform-view layers will not reach a KMS plane");
    return compositor;
  }
  if (impeller && compositor_mode_ != drm_config::Compositor::kPlanes) {
    ihs::log::warn(
        "[VulkanDrmBackend] Impeller: presenting through the root surface, not "
        "the compositor. Platform-view layers will not reach a KMS plane. With "
        "an engine that renders Impeller into Vulkan backing stores, pass "
        "--drm-compositor planes; otherwise run Skia if you need them.");
    return compositor;
  }
  if (impeller) {
    ihs::log::info(
        "[VulkanDrmBackend] Impeller: using the compositor (--drm-compositor "
        "planes). An engine without Impeller Vulkan backing stores presents "
        "nothing here.");
  }
#if BUILD_COMPOSITOR
  compositor.user_data = this;
  compositor.create_backing_store_callback = CreateBackingStoreCb;
  compositor.collect_backing_store_callback = CollectBackingStoreCb;
  compositor.present_layers_callback = PresentLayersCb;
  // Force a fresh backing store each frame so the engine cycles through the
  // scanout ring (rendering into a free buffer while KMS scans another) rather
  // than reusing one buffer — the basis for tear-free, vsync-paced present.
  compositor.avoid_backing_store_cache = true;
#else
  // BUILD_COMPOSITOR=OFF means no compositor, the same as the Impeller branch
  // above: the engine presents through the root surface instead. This used to
  // register the callbacks regardless, so the option compiled out the overlay
  // blend and the surface registry but left the engine on the compositor path
  // -- it changed what a layer stack could contain, not whether one was used.
  // drm_kms_egl has always guarded this block; this backend now matches.
  ihs::log::info(
      "[VulkanDrmBackend] built without BUILD_COMPOSITOR; presenting through "
      "the root surface. Platform-view layers will not reach a KMS plane.");
#endif
  return compositor;
}
