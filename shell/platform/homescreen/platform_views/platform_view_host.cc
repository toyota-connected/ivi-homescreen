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

#include "platform_view_host.h"

#include "config/common.h"  // BUILD_COMPOSITOR
#include "flutter_desktop_engine_state.h"
#include "logging/logging.h"

#include "ihs/platform_view.h"
#include "ihs/platform_view_host.h"

// The host bridges an ihs_pv plugin into the shell's compositor, so it only has
// work to do when the compositor is built. Without it, installing the host
// would register views the compositor can never place — make it a no-op
// instead.
#if BUILD_COMPOSITOR

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>  // std::size over the advertised format table
#include <map>
#include <memory>
#include <mutex>
#include <string>

#if IVI_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "backend/backend.h"
#if IVI_HAVE_VULKAN
#include "dmabuf_vulkan_import.h"
#endif
#if IVI_HAVE_EGL
#include "egl_dmabuf_import.h"
#endif
#include "flutter_desktop_view_controller_state.h"
#include "platform_view.h"
#include "platform_view_listener.h"
#include "platform_view_registry.h"
#include "view/compositor_surface_interface.h"
#include "view/flutter_view.h"

namespace {

// Ask the engine for a frame after a producer submits.
//
// A producer renders and submits on its own thread, out of band with Flutter's
// frame pipeline, but a platform-view surface is only composited from inside
// the embedder's PresentLayers callback — which runs when the engine presents a
// frame, and the engine only presents dirty ones. Flutter itself has nothing to
// repaint when just the platform-view content changed, so without this nudge
// the new frame sits unshown until something unrelated (input, a Dart repaint)
// drives a frame, which reads as a frozen view. The backends that scan a
// platform view out on their own vblank loop don't need this, but the nudge is
// harmless there. ScheduleFrame is thread-safe, so it is safe from the
// producer's thread (same reason the software cursor uses it).
//
// Submitting on its own thread also means a submit can land while the view is
// being torn down, and flutter_engine is never cleared on shutdown — a null
// check alone only covers "before the engine started" and would still hand a
// shut-down engine to ScheduleFrame. ~FlutterView latches shutting_down on the
// texture registrar before the engine state is destroyed for exactly this class
// of caller (the same gate the external-texture entry points use), so honor it
// here too.
void ScheduleEngineFrame(void* user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->flutter_engine == nullptr) {
    return;  // submit before the engine is running: nothing to nudge
  }
  if (state->texture_registrar == nullptr ||
      state->texture_registrar->shutting_down.load(std::memory_order_acquire)) {
    return;  // tearing down: the engine handle is no longer safe to call
  }
  LibFlutterEngine->ScheduleFrame(state->flutter_engine);
}

// Reaches the active Backend for the engine the host was installed for.
Backend* BackendOf(void* user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state != nullptr && state->view_controller != nullptr &&
      state->view_controller->view != nullptr) {
    return state->view_controller->view->GetBackend();
  }
  return nullptr;
}

// A registry-owned PlatformView that fronts an ihs_pv plugin: it holds the
// plugin's IhsPvCallbacks table and per-view state, trampolines the registry's
// platform_view_listener into that table, and is the ICompositorSurface the
// compositor pulls each frame. Submitted dma-bufs are imported into VkImages
// (cached per ring buffer) that GetVulkanImage hands the compositor; before the
// first submit it reports a null image ("nothing to composite this frame").
class IhsPluginView final : public PlatformView, public ICompositorSurface {
 public:
  explicit IhsPluginView(const PlatformViewRegistry::CreateRequest& request)
      : PlatformView(request.id,
                     request.view_type,
                     request.direction,
                     request.left,
                     request.top,
                     request.width,
                     request.height),
        id_(request.id) {}

  ~IhsPluginView() override;

  IhsPluginView(const IhsPluginView&) = delete;
  IhsPluginView& operator=(const IhsPluginView&) = delete;

  // Fire the plugin's dispose callback exactly once. A factory-created view can
  // be torn down either through the registry's dispose path or by its
  // RegisterListener "toggle" (a second create for the same id erases the owned
  // instance), and the plugin's dispose is what stops any producer thread that
  // still holds this view — so both paths must run it before the view's memory
  // is freed, or that thread use-after-frees the view. Platform-thread only.
  void DisposePlugin() {
    if (disposed_) {
      return;
    }
    disposed_ = true;
    if (callbacks.dispose != nullptr) {
      callbacks.dispose(plugin_user_data);
    }
  }

  // Plugin callback table + per-view state, filled by the factory.
  IhsPvCallbacks callbacks{};
  void* plugin_user_data{nullptr};

  // The backend, so dispose can hand imports to it for a safe deferred free
  // (see ~IhsPluginView). Set by the factory; may be null in headless contexts.
  Backend* backend_{nullptr};

  // Negotiated grant, cached from the host's grant() for the accessors.
  uint32_t granted_kind{IHS_PV_KIND_NONE};
  uint32_t drm_plane_id{0};
  int shm_fd{-1};
  size_t shm_stride{0};

  // The plugin submits from its own thread while the compositor samples on the
  // raster thread, so `mutex` guards the import block(s) below (Vulkan and/or
  // EGL — exactly one is populated per process).
  mutable std::mutex mutex;

#if IVI_HAVE_VULKAN
  // Imported dma-bufs keyed by the plugin's ring-buffer id — created once per
  // buffer, reused every submit. `current` points at the buffer the plugin most
  // recently submitted (a stable std::map node, so it survives later inserts).
  std::map<uint32_t, DmabufVulkanImporter::ImportedImage> buffers;
  DmabufVulkanImporter::ImportedImage* current{nullptr};
  uint32_t current_layout{VK_IMAGE_LAYOUT_UNDEFINED};
#endif

  // Common retire clock for both import paths (incremented on every submit).
  uint64_t submit_seq{0};

  // Synthesised buffer_id for a plugin whose IhsFrame predates that field, so
  // each of its frames is imported afresh instead of aliasing cache slot 0.
  // Atomic: written on the plugin thread in HostSubmit (outside v->mutex) and,
  // if a view ever sees concurrent submits, the increment must not tear.
  std::atomic<uint32_t> rolling_buffer_id{0};

  // Set once an id has been synthesised (the plugin's IhsFrame predates
  // buffer_id). Synthesised ids never repeat, so the import cache can never
  // reuse them; for such a view the import paths keep only the current import
  // and retire the rest, so the cache stays bounded instead of accumulating a
  // VkImage/GL texture (and its dma-buf) per frame for the producer's lifetime.
  // Atomic: written on the plugin thread (HostSubmit, outside v->mutex) and
  // read on the raster thread (GetGlTextureName / GetVulkanImage, under
  // v->mutex).
  std::atomic<bool> synth_buffer_id{false};

  // Deliver-once guard for the direct-scanout path: the submit_seq last handed
  // to GetDmabuf. The compositor commits faster than a 30fps producer submits,
  // so a reused overlay layer polls GetDmabuf every present; without this it
  // would re-import the same buffer each time, churning AddFB2/retire for no
  // new content. Set when GetDmabuf hands out a frame; a fresh submit bumps
  // submit_seq past it, re-arming delivery.
  mutable uint64_t dmabuf_delivered_seq{0};

#if IVI_HAVE_VULKAN
  // A resize re-creates a ring slot's dma-buf at a new size, replacing the
  // cached import. The old import can't be destroyed at once — a compositor
  // present command buffer may still bind it. It waits here until enough later
  // submits have gone by that any such frame has retired (frames retire in
  // order; the compositor runs only a few frames deep), then it is destroyed.
  struct RetiredImport {
    DmabufVulkanImporter::ImportedImage image;
    uint64_t reap_at{0};
  };
  std::vector<RetiredImport> retired;
#endif

#if IVI_HAVE_EGL
  // EGL/GL counterpart of the Vulkan import block above, used when the active
  // backend is EGL: the same submitted dma-buf is imported into a GL_TEXTURE_2D
  // (cached per ring buffer) that GetGlTextureName hands the EGL compositor.
  // Exactly one of the Vulkan or EGL block is populated per process (one
  // backend). Guarded by `mutex` like the Vulkan block.
  //
  // Unlike Vulkan, GL import (glGenTextures / glEGLImageTargetTexture2DOES) is
  // GL-context-affine, so it can't run in HostSubmit on the plugin's thread.
  // The producer's frame is stashed in `pending_egl` on submit; the actual
  // import happens lazily in GetGlTextureName, which the EGL compositor calls
  // on the raster thread with the context current. These fields are therefore
  // mutated from the const GetGlTextureName — hence `mutable` (guarded by
  // `mutex`).
  struct PendingEglFrame {
    bool valid{false};
    IhsFrame frame{};  // owns the plane fds until imported / superseded
    // Producer's acquire fence (sync_file fd) for this frame's writes.
    // GetDmabuf hands the DRM scene path a dup for the plane's IN_FENCE_FD and
    // this copy stays owned here; it is closed on a superseding submit, on
    // dispose, or by the GL-fallback wait in GetGlTextureName. -1 when the
    // producer synced before submit.
    int acquire_fence_fd{-1};
    // submit_seq when this frame was stashed. Compared against
    // dmabuf_delivered_seq to tell a frame the DRM scene path took (its release
    // rides OnScanoutRelease) from one superseded before it was ever scanned
    // out (its release eventfd must be signalled at supersede instead).
    uint64_t stash_seq{0};
  };
  mutable PendingEglFrame pending_egl;
  mutable std::map<uint32_t, EglDmabufImporter::ImportedTexture> buffers_egl;
  mutable EglDmabufImporter::ImportedTexture* current_egl{nullptr};
  struct RetiredEglImport {
    EglDmabufImporter::ImportedTexture texture;
    uint64_t reap_at{0};
  };
  mutable std::vector<RetiredEglImport> retired_egl;
#endif

  // Acquire fence (sync_file fd) for the latest submit, handed to the
  // compositor via TakeAcquireFenceFd. -1 when the producer stalled
  // synchronously. Owned here until taken; closed on the next submit that
  // supersedes it and on dispose.
  int pending_acquire_fd{-1};

  // Release fence (sync_file fd) from the compositor's most recent frame that
  // sampled this view. Polled at dispose so imports are not freed while a
  // compositor frame still binds them. -1 when none yet.
  int release_fence_fd{-1};

  // DRM scene (KMS plane) release path: a per-frame eventfd keyed by the
  // producer's buffer_id. HostSubmit creates one and hands the producer a dup
  // as its release fence; the compositor signals it (OnScanoutRelease) when the
  // plane stops scanning that frame out, so the producer reuses the slot only
  // then. Guarded by `mutex`. Any left over are closed at dispose.
  mutable std::map<uint32_t, int> release_efds;

  // ICompositorSurface — a Vulkan producer (no backing store, no GL texture).
  bool OnCreateBackingStore(const FlutterBackingStoreConfig*,
                            FlutterBackingStore*) override {
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override {
    return true;
  }
  bool OnPresent(const FlutterLayer*) override { return true; }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return id_;
  }
  void OnResize(int32_t, int32_t) override {}

#if IVI_HAVE_VULKAN
  // The compositor samples the most recently submitted buffer. Null until the
  // first frame arrives — the compositor treats that as "nothing this frame".
  [[nodiscard]] void* GetVulkanImage(int32_t* width,
                                     int32_t* height) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (current == nullptr || current->image == VK_NULL_HANDLE) {
      return nullptr;
    }
    if (width != nullptr) {
      *width = static_cast<int32_t>(current->width);
    }
    if (height != nullptr) {
      *height = static_cast<int32_t>(current->height);
    }
    return reinterpret_cast<void*>(current->image);
  }
  [[nodiscard]] uint32_t GetVulkanImageFormat() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current != nullptr ? static_cast<uint32_t>(current->format) : 0;
  }
  // Model/range are meaningful only for a YUV image; for an RGB one return the
  // interface default (0) rather than the ImportedImage field, so the reported
  // values match the ICompositorSurface contract and nothing keys off a stray
  // narrow-range/601 default on a frame that is never sampled through a
  // conversion.
  [[nodiscard]] uint32_t GetVulkanYcbcrModel() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current != nullptr && current->yuv
               ? static_cast<uint32_t>(current->ycbcr_model)
               : 0;
  }
  [[nodiscard]] uint32_t GetVulkanYcbcrRange() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current != nullptr && current->yuv
               ? static_cast<uint32_t>(current->ycbcr_range)
               : 0;
  }
  [[nodiscard]] uint32_t GetVulkanImageLayout() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current_layout;
  }
  void SetVulkanImageLayout(uint32_t layout) override {
    const std::lock_guard<std::mutex> lock(mutex);
    current_layout = layout;
  }
#endif

#if IVI_HAVE_EGL
  // GL seam consumed by the EGL backends (wayland-egl / drm-kms-egl). Imports
  // any frame stashed by HostSubmit into a GL_TEXTURE_2D on this (raster)
  // thread — the GL context is current here — then returns it. 0 until the
  // first frame. Defined out of line (needs g_egl_importer + CloseFrameFds).
  [[nodiscard]] uint32_t GetGlTextureName() const override;
  [[nodiscard]] bool TextureIsExternalOes() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current_egl != nullptr && current_egl->external;
  }
  [[nodiscard]] int32_t GetGlTextureWidth() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current_egl != nullptr ? static_cast<int32_t>(current_egl->width)
                                  : 0;
  }
  [[nodiscard]] int32_t GetGlTextureHeight() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current_egl != nullptr ? static_cast<int32_t>(current_egl->height)
                                  : 0;
  }
  // Imported dma-bufs are top-first (row 0 is the top), so the EGL compositor
  // must V-flip when sampling into its bottom-first framebuffer.
  [[nodiscard]] bool TextureIsTopFirst() const override { return true; }

  // Direct-scanout seam for the DRM compositor: expose the latest submitted
  // frame's dma-buf so it can be placed on a KMS overlay plane instead of being
  // GL-composited. Reads the frame stashed by HostSubmit and returns a *dup* of
  // its dma-buf fd — owned by the caller, which must close it. Duping under the
  // lock keeps the fd valid even if a concurrent HostSubmit supersedes and
  // closes the plugin's original before the compositor imports it. The
  // compositor still plane-routes XOR GL-imports a given surface per present so
  // the two paths don't both consume a frame. Producers submit top-down pixels
  // (no REFLECT_Y), so the plane scans the buffer out as-is.
  //
  // A GL fallback for a frame (no overlay plane fits) imports the stashed frame
  // via GetGlTextureName, which consumes it — so GetDmabuf then returns false
  // until the producer submits again. A continuous producer resumes direct
  // scanout on its next submit; a static producer stays GL-composited (still
  // correct, just not zero-copy) until it resubmits. Retaining the dma-buf
  // across a GL import so a static producer returns to scanout is part of the
  // dynamic-producer follow-up.
  [[nodiscard]] bool GetDmabuf(Dmabuf* out) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (out == nullptr || !pending_egl.valid) {
      return false;
    }
    // Hand each submitted frame to the scanout path at most once — the reuse
    // branch polls this every present but only wants a fresh buffer.
    if (dmabuf_delivered_seq == submit_seq) {
      return false;
    }
    const IhsFrame& f = pending_egl.frame;
    if (f.plane_count == 0 || f.plane_fd[0] < 0) {
      return false;
    }
    // Per-plane fds. A single-handle frame (v4l2/libcamera/Chromium) repeats
    // one fd across planes with distinct offsets; a multi-handle frame (e.g.
    // rpi-hevc-dec, which exports the Y and C planes as separate dma-bufs)
    // carries a distinct fd per plane. Dup each so the caller owns its copies;
    // AddFB2 resolves each to a GEM handle, so both layouts scan out. Clamp to
    // the 4 planes the descriptor holds (a DRM fourcc has at most 4).
    const uint32_t np = f.plane_count < 4 ? f.plane_count : 4;
    for (uint32_t i = 0; i < np; ++i) {
      if (f.plane_fd[i] < 0) {
        return false;  // a plane without a handle can't be scanned out
      }
    }
    int duped[4] = {-1, -1, -1, -1};
    for (uint32_t i = 0; i < np; ++i) {
      duped[i] = ::dup(f.plane_fd[i]);
      if (duped[i] < 0) {
        for (uint32_t j = 0; j < i; ++j) {
          ::close(duped[j]);
        }
        return false;
      }
    }
    for (uint32_t i = 0; i < np; ++i) {
      out->fd[i] = duped[i];  // owned by the caller
    }
    out->fourcc = f.format.fourcc;
    out->modifier = f.format.modifier;
    out->color_space = f.color_space;  // YUV colorimetry for the plane CSC
    out->color_range = f.color_range;
    out->width = f.width;
    out->height = f.height;
    out->plane_count = np;
    out->buffer_id =
        f.buffer_id;  // the compositor hands it to OnScanoutRelease
    for (uint32_t i = 0; i < np; ++i) {
      out->offset[i] = f.plane_offset[i];
      out->stride[i] = f.plane_stride[i];
    }
    // Explicit sync: hand the producer's acquire fence to the DRM scene path,
    // which wires it to the plane's IN_FENCE_FD (or CPU-waits as a fallback).
    // Per the ICompositorSurface::Dmabuf contract the surface keeps ownership
    // and the compositor gets a dup; retaining it here also lets the GL-texture
    // fallback (GetGlTextureName) still wait on the fence if this present later
    // falls back after GetDmabuf. The surface's copy is closed on the next
    // superseding submit, on dispose, or by the fallback wait. -1 when the
    // producer synced before submit.
    out->acquire_fence_fd = -1;
    if (pending_egl.acquire_fence_fd >= 0) {
      out->acquire_fence_fd = ::dup(pending_egl.acquire_fence_fd);
      if (out->acquire_fence_fd < 0) {
        // Fall back to implicit sync for this frame; log so it's diagnosable.
        ihs::log::warn(
            "[ihs_pv] dup(acquire_fence) failed (errno={}); implicit sync "
            "this frame",
            errno);
      }
    }
    dmabuf_delivered_seq = submit_seq;
    return true;
  }
#endif

  // The image is an imported dma-buf the plugin rewrites; the compositor
  // acquires it from VK_QUEUE_FAMILY_EXTERNAL each frame rather than using the
  // once-only transition for a compositor-owned image.
  [[nodiscard]] bool NeedsExternalQueueAcquire() const override { return true; }

  // Hand the compositor the acquire fence for the current buffer, transferring
  // ownership. -1 when the producer stalled synchronously (no wait needed).
  [[nodiscard]] int TakeAcquireFenceFd() override {
    const std::lock_guard<std::mutex> lock(mutex);
    const int fd = pending_acquire_fd;
    pending_acquire_fd = -1;
    return fd;
  }

  // Store the compositor's release fence for this view; it supersedes any
  // previous one (the newest frame's completion implies the older).
  void SetReleaseFenceFd(int fd) override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (release_fence_fd >= 0) {
      close(release_fence_fd);
    }
    release_fence_fd = fd;
  }

  // The DRM scene path retired this frame; wake the producer's release fence
  // for its ring slot. Compositor thread.
  void OnScanoutRelease(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mutex);
    SignalRelease(buffer_id);
  }

  // Signal and drop the release eventfd for `buffer_id` (a no-op if there is
  // none). Writing wakes the producer's poll on its dup; closing our copy
  // retires it. Caller holds `mutex`.
  void SignalRelease(uint32_t buffer_id) {
    const auto it = release_efds.find(buffer_id);
    if (it == release_efds.end()) {
      return;
    }
    // Should never fail for a live, blocking eventfd with a small counter, but
    // if it did the producer would stall forever on its dup with no clue why --
    // so log rather than swallow it.
    if (eventfd_write(it->second, 1) != 0) {
      ihs::log::warn(
          "[ihs_pv] eventfd_write(release bid={}) failed (errno={}); producer "
          "may stall on this ring slot",
          buffer_id, errno);
    }
    close(it->second);
    release_efds.erase(it);
  }

  // Create this frame's release eventfd, hand the producer a dup as its release
  // fence (via *out_fd), and keep our copy keyed on buffer_id to signal later.
  // A stale entry for the same slot is retired first. Caller holds `mutex`.
  void HandBackReleaseEventfd(uint32_t buffer_id, int* out_fd) {
    SignalRelease(buffer_id);  // retire any stale eventfd for this slot
    const int ef = eventfd(0, EFD_CLOEXEC);
    if (ef < 0) {
      ihs::log::warn(
          "[ihs_pv] eventfd() failed (errno={}); producer throttles "
          "conservatively this frame",
          errno);
      return;  // *out_fd stays -1
    }
    if (out_fd != nullptr) {
      const int dup_fd = ::dup(ef);
      if (dup_fd < 0) {
        ihs::log::warn("[ihs_pv] dup(release eventfd) failed (errno={})",
                       errno);
        close(ef);  // don't leak ef or leave a stale release_efds entry the
        return;     // producer never got a fence to wait on
      }
      *out_fd = dup_fd;
    }
    release_efds[buffer_id] = ef;
  }

 private:
  int32_t id_;
  bool disposed_{false};
};

#if IVI_HAVE_VULKAN
// One importer per process (a single Vulkan device), initialized once from the
// backend's Vulkan context at host install. Stays not-ready when the active
// backend is not Vulkan or lacks the dma-buf import extensions, in which case
// ihs_pv views fall back to producing no frame.
DmabufVulkanImporter g_importer;
#endif

#if IVI_HAVE_EGL
// EGL/GL counterpart of g_importer, initialized from the backend's EGL context
// at host install when the active backend is EGL (and Vulkan is absent). Stays
// not-ready on a Vulkan backend.
EglDmabufImporter g_egl_importer;
#endif

IhsPluginView::~IhsPluginView() {
  // Stop any producer thread that still holds this view BEFORE freeing the
  // imports it submits into — run outside the lock, since the dispose join may
  // let that thread take the lock (GetVulkanImage/submit) on its way out.
  DisposePlugin();
  const std::lock_guard<std::mutex> lock(mutex);
  if (release_fence_fd >= 0) {
    close(release_fence_fd);
    release_fence_fd = -1;
  }
  if (pending_acquire_fd >= 0) {
    close(pending_acquire_fd);
    pending_acquire_fd = -1;
  }
  // The producer has been stopped (DisposePlugin above), so just retire any
  // release eventfds it never got to wait on; it owns and closes its own dups.
  for (const auto& [buffer_id, fd] : release_efds) {
    if (fd >= 0) {
      close(fd);
    }
  }
  release_efds.clear();
#if IVI_HAVE_VULKAN
  // With explicit-sync acquire the compositor's read of these imports is gated
  // on the producer's fence, so a re-create can dispose this view while a
  // compositor frame is still recording or in flight binding them (a GPU
  // completion fence can't cover a not-yet-submitted record). Hand the frees to
  // the backend, which runs them at the top of a later present — after prior
  // frames' fences and before recording — so no frame binds a freed image.
  const auto defer = [this](DmabufVulkanImporter::ImportedImage img) {
    if (backend_ != nullptr) {
      backend_->ScheduleDeferredDestroy(
          [img]() mutable { g_importer.Destroy(&img); });
    } else {
      g_importer.Destroy(&img);
    }
  };
  for (auto& [buffer_id, image] : buffers) {
    defer(image);
  }
  buffers.clear();
  for (auto& r : retired) {
    defer(r.image);
  }
  retired.clear();
#endif

#if IVI_HAVE_EGL
  // A frame stashed but never imported (submitted with no present before
  // dispose) still owns its dma-buf fds — close them so they don't leak.
  // (CloseFrameFds is defined below this out-of-line dtor, so close inline.)
  if (pending_egl.valid) {
    for (uint32_t i = 0; i < pending_egl.frame.plane_count && i < 4; ++i) {
      if (pending_egl.frame.plane_fd[i] >= 0) {
        close(pending_egl.frame.plane_fd[i]);
      }
    }
    if (pending_egl.acquire_fence_fd >= 0) {
      close(pending_egl.acquire_fence_fd);
      pending_egl.acquire_fence_fd = -1;
    }
    pending_egl.valid = false;
  }
  // Same deferred free for the GL imports (glDeleteTextures needs the GL
  // context current, which the backend's deferred-destroy runs at a present).
  const auto defer_egl = [this](EglDmabufImporter::ImportedTexture tex) {
    if (backend_ != nullptr) {
      backend_->ScheduleDeferredDestroy(
          [tex]() mutable { g_egl_importer.Destroy(&tex); });
    } else {
      g_egl_importer.Destroy(&tex);
    }
  };
  for (auto& [buffer_id, tex] : buffers_egl) {
    defer_egl(tex);
  }
  buffers_egl.clear();
  for (auto& r : retired_egl) {
    defer_egl(r.texture);
  }
  retired_egl.clear();
#endif
}

// Close every plane fd a frame still owns (its import did not consume them).
void CloseFrameFds(const IhsFrame* frame) {
  // One fd may back several planes (IhsFrame contract), so a plane_fd can
  // repeat across entries. Close each distinct numeric fd once -- closing a
  // duplicate a second time could close an unrelated fd that reused the number.
  for (uint32_t i = 0; i < frame->plane_count && i < 4; ++i) {
    if (frame->plane_fd[i] < 0) {
      continue;
    }
    bool already_closed = false;
    for (uint32_t j = 0; j < i; ++j) {
      if (frame->plane_fd[j] == frame->plane_fd[i]) {
        already_closed = true;
        break;
      }
    }
    if (!already_closed) {
      close(frame->plane_fd[i]);
    }
  }
}

#if IVI_HAVE_EGL
// Raster-thread lazy import for the EGL path: HostSubmit stashed the frame
// (plugin thread, no GL context); import it here, where the EGL compositor
// calls us with the context current, caching per ring buffer like the Vulkan
// path.
uint32_t IhsPluginView::GetGlTextureName() const {
  std::unique_lock<std::mutex> lock(mutex);
  if (pending_egl.valid) {
    // The GL-texture fallback samples the buffer directly with no plane
    // IN_FENCE_FD, so block until the producer's writes complete before
    // sampling, then release the fence. Only reached when direct scanout is
    // unavailable (the fence otherwise rides the plane's IN_FENCE_FD via
    // GetDmabuf), so this wait is off the hot path. Drop the lock across the
    // (bounded) wait so a wedged fence can't block HostSubmit or dispose; take
    // ownership of the fd first so a superseding submit won't close it, and
    // loop so a frame that arrived while unlocked is also waited on — we never
    // sample ahead of the producer. On timeout/error we sample best-effort but
    // warn.
    while (pending_egl.valid && pending_egl.acquire_fence_fd >= 0) {
      const int fence = pending_egl.acquire_fence_fd;
      pending_egl.acquire_fence_fd = -1;
      lock.unlock();
      pollfd pfd{fence, POLLIN, 0};
      int pr = 0;
      while ((pr = ::poll(&pfd, 1, 1000)) < 0 && errno == EINTR) {
      }
      // Only POLLIN means the fence signalled; poll() can also wake on
      // POLLERR/POLLHUP/POLLNVAL (>0 with no POLLIN), which is a failure, not a
      // signal. Sample best-effort either way, but warn.
      if (pr <= 0 || (pfd.revents & POLLIN) == 0) {
        ihs::log::warn(
            "[ihs_pv] GL-fallback acquire-fence wait {} (fd={}, "
            "revents=0x{:x});"
            " sampling anyway — frame may tear",
            pr == 0 ? "timed out" : "failed", fence, pfd.revents);
      }
      close(fence);
      lock.lock();
    }
    // The view may have been disposed while the lock was dropped for the wait.
    if (!pending_egl.valid) {
      return current_egl != nullptr ? current_egl->texture : 0;
    }
    // Margin (in submits) before a resize-replaced import is destroyed; the
    // compositor samples GetGlTextureName synchronously here, so GL's own
    // deferred deletion already covers in-flight draws — the margin is safety.
    constexpr uint64_t kImportRetireMargin = 8;
    for (auto rit = retired_egl.begin(); rit != retired_egl.end();) {
      if (submit_seq >= rit->reap_at) {
        g_egl_importer.Destroy(&rit->texture);  // GL context current here
        rit = retired_egl.erase(rit);
      } else {
        ++rit;
      }
    }
    IhsFrame& f = pending_egl.frame;
    const auto it = buffers_egl.find(f.buffer_id);
    if (it != buffers_egl.end() && it->second.width == f.width &&
        it->second.height == f.height) {
      CloseFrameFds(&f);  // redundant handle to the cached import
      current_egl = &it->second;
    } else {
      if (it != buffers_egl.end()) {
        retired_egl.push_back({it->second, submit_seq + kImportRetireMargin});
        buffers_egl.erase(it);
      }
      EglDmabufImporter::ImportedTexture imported;
      if (g_egl_importer.Import(f, &imported)) {
        auto [pos, ins] = buffers_egl.emplace(f.buffer_id, imported);
        current_egl = &pos->second;
        // Synthesised ids never repeat, so every earlier entry is dead. Retire
        // them (the reap margin covers a compositor present still binding one)
        // so the cache holds just the current import rather than growing per
        // frame.
        if (synth_buffer_id.load(std::memory_order_relaxed)) {
          for (auto bit = buffers_egl.begin(); bit != buffers_egl.end();) {
            if (bit->first != f.buffer_id) {
              retired_egl.push_back(
                  {bit->second, submit_seq + kImportRetireMargin});
              bit = buffers_egl.erase(bit);
            } else {
              ++bit;
            }
          }
        }
      } else {
        CloseFrameFds(&f);  // import left the fds untouched on failure
      }
    }
    pending_egl.valid = false;
  }
  return current_egl != nullptr ? current_egl->texture : 0;
}
#endif

// platform_view_listener trampolines: the registry drives these with the
// listener context (the IhsPluginView*), which we forward into the plugin's
// IhsPvCallbacks. accept_gesture/reject_gesture take a bare id (no context), so
// they cannot reach the view here and are left null — gesture-arena arbitration
// for ihs_pv views is a follow-up.
void ListenerResize(double width, double height, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.resize != nullptr) {
    view->callbacks.resize(view->plugin_user_data, width, height);
  }
}

void ListenerOnTouch(int32_t action,
                     int32_t point_count,
                     size_t pointer_data_size,
                     const double* pointer_data,
                     void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.on_touch != nullptr) {
    view->callbacks.on_touch(view->plugin_user_data, action, point_count,
                             pointer_data_size, pointer_data);
  }
}

void ListenerDispose(bool /*hybrid*/, void* data) {
  static_cast<IhsPluginView*>(data)->DisposePlugin();
}

const platform_view_listener kListener = {
    /* resize */ ListenerResize,
    /* set_direction */ nullptr,
    /* set_offset */ nullptr,
    /* on_touch */ ListenerOnTouch,
    /* dispose */ ListenerDispose,
    /* accept_gesture */ nullptr,
    /* reject_gesture */ nullptr,
};

// --- IhsPvHost implementation -----------------------------------------------

int HostRegisterFactory(void* user_data,
                        const char* view_type,
                        IhsPvFactory factory,
                        void* factory_user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->platform_view_registry == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  const std::string type(view_type);
  state->platform_view_registry->RegisterFactory(
      type,
      [factory, factory_user_data, state](
          PlatformViewRegistry& registry,
          const PlatformViewRegistry::CreateRequest& request)
          -> std::unique_ptr<PlatformView> {
        auto view = std::make_unique<IhsPluginView>(request);
        view->backend_ = BackendOf(state);

        IhsPvCreateInfo info{};
        info.struct_size = sizeof(info);
        info.id = request.id;
        info.view_type = request.view_type.c_str();
        info.direction = request.direction;
        info.left = request.left;
        info.top = request.top;
        info.width = request.width;
        info.height = request.height;
        if (request.params != nullptr && !request.params->empty()) {
          info.params = request.params->data();
          info.params_size = request.params->size();
        }

        const int rc = factory(&info, factory_user_data,
                               reinterpret_cast<IhsPlatformView*>(view.get()),
                               &view->callbacks, &view->plugin_user_data);
        if (rc != IHS_PV_OK) {
          ihs::log::warn("[ihs_pv] factory for '{}' refused (rc={})",
                         request.view_type, rc);
          return nullptr;
        }

        // Drive lifecycle through the registry's listener table, and register
        // the compositor surface so the backend pulls frames. The surface's
        // lifetime is the registry-owned instance; the compositor holds a
        // non-owning alias dropped on UnregisterCompositorSurface at dispose.
        registry.RegisterListener(request.id, &kListener, view.get());
        if (state->view_controller != nullptr &&
            state->view_controller->view != nullptr) {
          state->view_controller->view->RegisterCompositorSurface(
              request.id, std::shared_ptr<ICompositorSurface>(
                              view.get(), [](ICompositorSurface*) {}));
        }
        return view;
      });
  return IHS_PV_OK;
}

void HostUnregisterFactory(void* user_data, const char* view_type) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state != nullptr && state->platform_view_registry != nullptr) {
    state->platform_view_registry->UnregisterFactory(std::string(view_type));
  }
}

// The packed-RGB fourccs every import path here accepts unconditionally: the
// Vulkan importer maps all four to a VkFormat, and the EGL importer takes them
// on a plain GL_TEXTURE_2D (its format switch only separates YUV, which needs
// GL_TEXTURE_EXTERNAL_OES).
//
// This list exists so a plugin that passes no formats -- documented as "any the
// backend offers" -- gets told which one it got. Without it caps.format_count
// is 0, choose_format falls through to a zeroed IhsFormatModifier, and the
// grant reports fourcc 0: a plugin then either guesses or allocates against
// zero, which gbm accepts and eglCreateImageKHR later rejects with
// EGL_BAD_MATCH, surfacing as a driver fault far from the cause.
//
// LINEAR per the IhsFormatModifier contract ("DRM_FORMAT_MOD_LINEAR when
// unsure"): it is what every importer can read, and scanout modifier
// negotiation is a separate step the grant already carries. YUV producers are
// unaffected -- they name their format explicitly rather than asking for any.
constexpr uint32_t HostFourcc(const char a,
                              const char b,
                              const char c,
                              const char d) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
         (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

constexpr IhsFormatModifier kDmabufImportFormats[] = {
    {HostFourcc('X', 'R', '2', '4'), 0, 0},  // DRM_FORMAT_XRGB8888, LINEAR
    {HostFourcc('A', 'R', '2', '4'), 0, 0},  // DRM_FORMAT_ARGB8888, LINEAR
    {HostFourcc('X', 'B', '2', '4'), 0, 0},  // DRM_FORMAT_XBGR8888, LINEAR
    {HostFourcc('A', 'B', '2', '4'), 0, 0},  // DRM_FORMAT_ABGR8888, LINEAR
};

int HostQueryCapabilities(void* user_data, IhsPvCapabilities* out) {
  out->backend_key = "";
  out->kinds =
      IHS_PV_KIND_SOFTWARE_SHM;  // the universal floor is always offered
  Backend* backend = BackendOf(user_data);
  if (backend != nullptr) {
    BackendVulkanContext vk{};
    if (backend->GetVulkanContext(&vk)) {
      out->kinds |= IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
      // Explicit-sync acquire is available when the shared device can import a
      // producer's sync_file as a semaphore — the compositor's platform-view
      // wait path (TakeAcquireFenceFd -> vkImportSemaphoreFdKHR) needs
      // VK_KHR_external_semaphore_fd. Advertise it so a Vulkan producer hands
      // over a fence instead of stalling on the blit.
      for (size_t i = 0; i < vk.device_extension_count; ++i) {
        if (vk.device_extensions[i] != nullptr &&
            std::strcmp(vk.device_extensions[i],
                        "VK_KHR_external_semaphore_fd") == 0) {
          out->explicit_sync = 1;
          break;
        }
      }
    }
#if IVI_HAVE_EGL
    // The EGL backends import a submitted dma-buf into a GL_TEXTURE_2D, so the
    // dma-buf-import kind is offered on a GL context too (a GLES producer
    // path).
    BackendEglContext egl{};
    if (backend->GetEglContext(&egl)) {
      out->kinds |= IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
      // The DRM-KMS-EGL backend (gbm_device set; wayland-egl leaves it null)
      // runs the plane compositor, which can scan out a submitted dma-buf
      // directly on a KMS overlay plane — offer the zero-copy DRM_PLANE kind.
      // The scene path falls back to GL if a plane can't be allocated for a
      // given frame, so advertising it never hard-fails a view.
      if (egl.gbm_device != nullptr) {
        out->kinds |= IHS_PV_KIND_DRM_PLANE;
      }
    }
#endif
  }
  // Name the formats behind the dma-buf kinds. Only when one was actually
  // offered: advertising formats for a capability the backend does not have
  // would hand a plugin a format it can never submit.
  if ((out->kinds & IHS_PV_KIND_TEXTURE_DMABUF_IMPORT) != 0U ||
      (out->kinds & IHS_PV_KIND_DRM_PLANE) != 0U) {
    out->formats = kDmabufImportFormats;
    out->format_count = std::size(kDmabufImportFormats);
  }
  return IHS_PV_OK;
}

int HostVulkanContext(void* user_data, IhsVulkanContext* out) {
  Backend* backend = BackendOf(user_data);
  BackendVulkanContext vk{};
  if (backend == nullptr || !backend->GetVulkanContext(&vk)) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  out->instance = vk.instance;
  out->physical_device = vk.physical_device;
  out->device = vk.device;
  out->queue = vk.queue;
  out->queue_family_index = vk.queue_family_index;
  out->get_instance_proc_addr = vk.get_instance_proc_addr;
  // Device extensions let the plugin gate optional paths (e.g. sync_file
  // acquire) on the shared device's capabilities. Storage is backend-owned and
  // outlives this context.
  out->device_extensions = vk.device_extensions;
  out->device_extension_count = vk.device_extension_count;
  // Instance extensions, queue lock, and the VMA allocator are not exposed by
  // the backend yet; the plugin allocates manually until then.
  return IHS_PV_OK;
}

int HostEglContext(void* user_data, IhsEglContext* out) {
  Backend* backend = BackendOf(user_data);
  BackendEglContext egl{};
  if (backend == nullptr || !backend->GetEglContext(&egl)) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  out->egl_display = egl.display;
  out->egl_context = egl.share_context;
  out->egl_config = egl.config;
  out->gbm_device = egl.gbm_device;
  return IHS_PV_OK;
}

int HostGrant(void* /*user_data*/,
              IhsPlatformView* view,
              uint32_t kind,
              const IhsFormatModifier* /*format*/,
              uint32_t* /*out_drm_plane_id*/,
              int* /*out_shm_fd*/,
              size_t* /*out_shm_stride*/) {
  // Record the granted kind on the view. Reserving a DRM plane / shm buffer for
  // the non-Vulkan kinds is wired with the submit path; the Vulkan
  // texture-import kind needs no pull-side reservation.
  reinterpret_cast<IhsPluginView*>(view)->granted_kind = kind;
  return IHS_PV_OK;
}

void HostRevoke(void* /*user_data*/, IhsPlatformView* view) {
  reinterpret_cast<IhsPluginView*>(view)->granted_kind = IHS_PV_KIND_NONE;
}

uint32_t HostGrantDrmPlaneId(void* /*user_data*/, IhsPlatformView* view) {
  return reinterpret_cast<IhsPluginView*>(view)->drm_plane_id;
}

int HostGrantShmFd(void* /*user_data*/,
                   IhsPlatformView* view,
                   size_t* out_stride) {
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  if (out_stride != nullptr) {
    *out_stride = v->shm_stride;
  }
  return v->shm_fd;
}

// Hand back a dup of the compositor's latest release fence for this view
// (#338), so a producer on an explicit-sync grant can wait on it before reusing
// a ring buffer. Only explicit-sync submits get a fence: ihs_pv_submit's
// contract keeps *out_release_fence_fd at -1 on implicit paths (where release
// rides the callback / native protocol), and a submit is implicit exactly when
// it carries no acquire fence. Also leaves it at -1 when there is no fence yet
// or the dup fails -- the producer then throttles conservatively rather than
// racing the compositor. Caller holds v->mutex (release_fence_fd is read here).
void HandBackReleaseFence(const IhsPluginView* v,
                          int acquire_fence_fd,
                          int* out_release_fence_fd) {
  if (out_release_fence_fd == nullptr || acquire_fence_fd < 0 ||
      v->release_fence_fd < 0) {
    return;
  }
  const int fd = ::dup(v->release_fence_fd);
  if (fd < 0) {
    // Capture errno before the log call, which may clobber it, then log so an
    // explicit-sync stall is diagnosable, matching the acquire path.
    const int dup_errno = errno;
    ihs::log::warn(
        "[ihs_pv] dup(release_fence) failed (errno={}); producer throttles "
        "conservatively this frame",
        dup_errno);
    return;
  }
  *out_release_fence_fd = fd;
}

int HostSubmit(void* user_data,
               IhsPlatformView* view,
               const IhsFrame* frame,
               int acquire_fence_fd,
               int* out_release_fence_fd) {
  // Release fence (compositor -> producer, #338): -1 by default, replaced under
  // v->mutex in each backend path below with a dup of this view's latest fence
  // (see HandBackReleaseFence). -1 stands until the first composite has run, or
  // if the dup fails; the producer owns and closes any fd handed back.
  if (out_release_fence_fd != nullptr) {
    *out_release_fence_fd = -1;
  }

  auto* v = reinterpret_cast<IhsPluginView*>(view);

  // The plugin may be built against an older, smaller IhsFrame. Copying *frame
  // whole would read past the end of its object, and the trailing buffer_id
  // (the import-cache key) would be garbage. Copy only the bytes it provided
  // into a full-size zeroed frame; fields its struct did not reach read as
  // zero. A struct too small to even carry the plane data is rejected -- and
  // not closed, since plane_count/plane_fd cannot be trusted either.
  IhsFrame normalized{};
  if (frame == nullptr ||
      frame->struct_size <
          offsetof(IhsFrame, plane_stride) + sizeof(normalized.plane_stride)) {
    if (acquire_fence_fd >= 0) {
      close(acquire_fence_fd);
    }
    ihs::log::warn(
        "[ihs_pv] submit rejected: IhsFrame struct_size {} too small",
        frame != nullptr ? frame->struct_size : 0);
    return IHS_PV_ERR_INVALID;
  }
  const size_t copy = frame->struct_size < sizeof(IhsFrame) ? frame->struct_size
                                                            : sizeof(IhsFrame);
  std::memcpy(&normalized, frame, copy);
  normalized.struct_size = sizeof(IhsFrame);
  // buffer_id absent from the plugin's struct read as 0, which would key every
  // frame on one cache slot and freeze on the first. Give each such frame a
  // fresh id so it is imported rather than aliased.
  if (frame->struct_size <
      offsetof(IhsFrame, buffer_id) + sizeof(normalized.buffer_id)) {
    normalized.buffer_id =
        v->rolling_buffer_id.fetch_add(1, std::memory_order_relaxed);
    v->synth_buffer_id.store(true, std::memory_order_relaxed);
  }
  frame = &normalized;

#if IVI_HAVE_VULKAN
  const bool vulkan_ready = g_importer.ready();
#else
  constexpr bool vulkan_ready = false;
#endif

#if IVI_HAVE_EGL
  if (!vulkan_ready && g_egl_importer.ready()) {
    // EGL backend: GL import is context-affine, so only stash the frame here
    // (on the plugin's thread); GetGlTextureName imports it on the raster
    // thread. The DRM scene path (GetDmabuf) forwards the acquire fence to the
    // plane's IN_FENCE_FD for explicit sync; the GL-texture fallback has no
    // plane fence, so GetGlTextureName CPU-waits on it before sampling. Stash
    // the fence with the frame for both.
    std::unique_lock<std::mutex> lock(v->mutex);
    ++v->submit_seq;
    HandBackReleaseFence(v, acquire_fence_fd, out_release_fence_fd);
    if (v->pending_egl.valid) {
      // A frame the DRM scene path never took (superseded before GetDmabuf
      // delivered it) is never scanned out, so no OnScanoutRelease will fire
      // for it -- signal its release here so the producer reclaims that slot. A
      // frame that WAS delivered rides OnScanoutRelease and must not be
      // double-signalled.
      if (v->dmabuf_delivered_seq < v->pending_egl.stash_seq) {
        v->SignalRelease(v->pending_egl.frame.buffer_id);
      }
      CloseFrameFds(&v->pending_egl.frame);  // superseded before import
      if (v->pending_egl.acquire_fence_fd >= 0) {
        close(v->pending_egl.acquire_fence_fd);
      }
    }
    v->pending_egl.frame = *frame;       // take ownership of the plane fds
    v->pending_egl.frame.hdr = nullptr;  // do not retain the plugin's hdr ptr
    v->pending_egl.acquire_fence_fd = acquire_fence_fd;  // ownership moves here
    v->pending_egl.stash_seq = v->submit_seq;
    v->pending_egl.valid = true;
    // Per-frame release eventfd: hand the producer a dup to wait on before it
    // reuses this ring slot; the compositor signals our copy from
    // OnScanoutRelease. A stale entry for this buffer_id (the producer reused
    // the slot without our having signalled) is retired first.
    v->HandBackReleaseEventfd(frame->buffer_id, out_release_fence_fd);
    lock.unlock();  // don't call into the engine holding the view lock
    ScheduleEngineFrame(user_data);
    return IHS_PV_OK;
  }
#endif

#if !IVI_HAVE_VULKAN
  // No Vulkan importer compiled and EGL (if any) did not handle it: no backend
  // consumes the frame.
  (void)vulkan_ready;
  CloseFrameFds(frame);
  if (acquire_fence_fd >= 0) {
    close(acquire_fence_fd);
  }
  return IHS_PV_ERR_NO_BACKEND;
}
#else
  if (!vulkan_ready) {
    CloseFrameFds(frame);
    if (acquire_fence_fd >= 0) {
      close(acquire_fence_fd);
    }
    return IHS_PV_ERR_NO_BACKEND;
  }

  // Margin, in submits, before a resize-replaced import is destroyed — safely
  // more than the frames the compositor keeps in flight, so no present command
  // buffer still binds it by the time it is freed.
  constexpr uint64_t kImportRetireMargin = 8;

  std::unique_lock<std::mutex> lock(v->mutex);
  ++v->submit_seq;
  HandBackReleaseFence(v, acquire_fence_fd, out_release_fence_fd);
  // Stash this frame's acquire fence (a sync_file) for the compositor to wait
  // on before sampling; it supersedes any previous unconsumed one, since the
  // compositor only ever samples the latest submit (v->current). -1 is an
  // implicit-sync submit -- no acquire fence, the producer already made the
  // content ready -- which is the same condition under which
  // HandBackReleaseFence hands back no release fence.
  if (v->pending_acquire_fd >= 0) {
    close(v->pending_acquire_fd);
  }
  v->pending_acquire_fd = acquire_fence_fd;

  // Reap imports whose retire margin has elapsed. This runs on the plugin's
  // thread, so hand the free to the backend rather than destroying here — the
  // compositor may still be recording a frame that binds the image, which a
  // submit-count margin alone doesn't cover.
  for (auto rit = v->retired.begin(); rit != v->retired.end();) {
    if (v->submit_seq >= rit->reap_at) {
      if (v->backend_ != nullptr) {
        auto img = rit->image;
        v->backend_->ScheduleDeferredDestroy(
            [img]() mutable { g_importer.Destroy(&img); });
      } else {
        g_importer.Destroy(&rit->image);
      }
      rit = v->retired.erase(rit);
    } else {
      ++rit;
    }
  }

  const auto it = v->buffers.find(frame->buffer_id);
  if (it != v->buffers.end() && it->second.width == frame->width &&
      it->second.height == frame->height) {
    // Known ring buffer, unchanged size: the submitted fd is a redundant handle
    // to the same memory. Close it and reuse the existing import.
    CloseFrameFds(frame);
    v->current = &it->second;
  } else {
    // New ring id, or the plugin re-created this slot at a different size (a
    // resize): the cached import — if any — now aliases old memory of the wrong
    // dimensions, so retire it and import the submitted dma-buf. Otherwise
    // GetVulkanImage keeps handing the compositor the stale image and the blend
    // just scales it to the new view rect. The old import is retired (not freed
    // now) because a compositor present may still bind it this frame.
    if (it != v->buffers.end()) {
      v->retired.push_back({it->second, v->submit_seq + kImportRetireMargin});
      v->buffers.erase(it);
    }
    DmabufVulkanImporter::ImportedImage imported;
    if (!g_importer.Import(*frame, &imported)) {
      CloseFrameFds(frame);  // import left the fds untouched on failure
      return IHS_PV_ERR_INVALID;
    }
    // Import consumed plane_fd[0]; a single-plane RGB frame owns no other fds.
    auto [pos, inserted] = v->buffers.emplace(frame->buffer_id, imported);
    v->current = &pos->second;
    // Synthesised ids never repeat, so every earlier entry is dead. Retire them
    // (the reap margin covers a compositor present still binding one) so the
    // cache holds just the current import rather than growing per frame.
    if (v->synth_buffer_id.load(std::memory_order_relaxed)) {
      for (auto bit = v->buffers.begin(); bit != v->buffers.end();) {
        if (bit->first != frame->buffer_id) {
          v->retired.push_back(
              {bit->second, v->submit_seq + kImportRetireMargin});
          bit = v->buffers.erase(bit);
        } else {
          ++bit;
        }
      }
    }
  }

  // The plugin re-rendered the buffer before submitting, so the compositor
  // transitions from GENERAL to read it. A spec-correct foreign-queue-family
  // acquire from the producer is the explicit-sync increment.
  v->current_layout = VK_IMAGE_LAYOUT_GENERAL;
  lock.unlock();  // don't call into the engine holding the view lock
  ScheduleEngineFrame(user_data);
  return IHS_PV_OK;
}
#endif  // IVI_HAVE_VULKAN

// Process-global host; user_data re-points at the most recently installed
// engine (single-engine today).
IhsPvHost g_host{};

}  // namespace

void InstallPlatformViewHost(FlutterDesktopEngineState* engine_state) {
  if (engine_state == nullptr ||
      engine_state->platform_view_registry == nullptr) {
    return;
  }
  g_host.struct_size = sizeof(g_host);
  g_host.user_data = engine_state;
  g_host.register_factory = HostRegisterFactory;
  g_host.unregister_factory = HostUnregisterFactory;
  g_host.query_capabilities = HostQueryCapabilities;
  g_host.vulkan_context = HostVulkanContext;
  g_host.egl_context = HostEglContext;
  g_host.grant = HostGrant;
  g_host.revoke = HostRevoke;
  g_host.grant_drm_plane_id = HostGrantDrmPlaneId;
  g_host.grant_shm_fd = HostGrantShmFd;
  g_host.submit = HostSubmit;
  ihs_pv_set_host(&g_host);

  // Bring up the dma-buf importer once, on this thread, from the backend's
  // Vulkan or EGL context — so the submit path (which runs off-thread) never
  // races an init. A backend provides exactly one of the two, so these are
  // independent (not else-if). Not-ready is fine: the view produces no frame.
  if (Backend* backend = BackendOf(engine_state); backend != nullptr) {
    (void)backend;
#if IVI_HAVE_VULKAN
    if (BackendVulkanContext vk{}; backend->GetVulkanContext(&vk)) {
      g_importer.Init(static_cast<VkInstance>(vk.instance),
                      static_cast<VkPhysicalDevice>(vk.physical_device),
                      static_cast<VkDevice>(vk.device),
                      vk.get_instance_proc_addr);
    }
#endif
#if IVI_HAVE_EGL
    // On an EGL backend, bring up the GL importer from the backend's
    // EGLDisplay. Only the entry points are resolved here (no GL context
    // needed); the imports themselves happen on the raster thread.
    if (BackendEglContext egl{}; backend->GetEglContext(&egl)) {
      g_egl_importer.Init(egl.display);
    }
#endif
  }
#if IVI_HAVE_VULKAN && IVI_HAVE_EGL
  ihs::log::debug(
      "[ihs_pv] platform-view host installed (dma-buf import: vulkan {}, egl "
      "{})",
      g_importer.ready() ? "ready" : "off",
      g_egl_importer.ready() ? "ready" : "off");
#elif IVI_HAVE_VULKAN
  ihs::log::debug(
      "[ihs_pv] platform-view host installed (dma-buf import: vulkan {})",
      g_importer.ready() ? "ready" : "unavailable");
#elif IVI_HAVE_EGL
  ihs::log::debug(
      "[ihs_pv] platform-view host installed (dma-buf import: egl {})",
      g_egl_importer.ready() ? "ready" : "unavailable");
#else
  ihs::log::debug(
      "[ihs_pv] platform-view host installed (no dma-buf import backend)");
#endif
}

#else  // !BUILD_COMPOSITOR

void InstallPlatformViewHost(FlutterDesktopEngineState* /*engine_state*/) {}

#endif  // BUILD_COMPOSITOR
