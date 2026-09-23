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
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
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
#include <vector>

#if IVI_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "asio/post.hpp"

#include "backend/backend.h"
#include "deferred_retire_set.h"
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
#include "task_runner.h"
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
  // Marshal onto the platform thread (#623). ihs_pv_submit is any-thread by
  // contract and producers use that -- but FlutterEngineScheduleFrame is
  // platform-thread-only in the engine, implicitly: it dereferences the
  // shell's fml::WeakPtr<PlatformView>, which checks its creation thread, and
  // Shell::OnPlatformViewScheduleFrame asserts the platform runner outright.
  // Both are FML_DCHECKs, so a release engine never complains and a debug one
  // asserts on the first producer frame; the release hazard is the WeakPtr
  // deref racing platform-view teardown.
  //
  // Posting rather than blocking: the producer's thread carries a decode or
  // render loop and has no business waiting on ours. ScheduleFrame is
  // idempotent -- Animator::RequestFrame coalesces on a semaphore -- so a
  // nudge that lands a beat late, or twice, costs nothing.
  TaskRunner* runner = state->platform_task_runner;
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return;  // no runner yet; the next submit nudges
  }
  asio::post(*runner->GetStrandContext(), [state]() {
    // Re-check on the platform thread: teardown may have run while this was
    // queued, and that is the thread which latches it.
    if (state->flutter_engine == nullptr ||
        state->texture_registrar == nullptr ||
        state->texture_registrar->shutting_down.load(
            std::memory_order_acquire)) {
      return;
    }
    LibFlutterEngine->ScheduleFrame(state->flutter_engine);
  });
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
  // The KMS plane this view was scanned out on at the last present, or 0 when
  // it was GL-composited (no plane) that present. Written on the compositor
  // thread via SetScanoutPlane, read on the platform thread via the DRM_PLANE
  // accessor, so it is atomic. Feeds ihs_pv_grant_drm_plane_id.
  std::atomic<uint32_t> drm_plane_id{0};
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

  // The producer's dma-buf kept alive beside the import, so a frame can reach
  // a KMS plane as well as be sampled. The importer consumes the submitted fds
  // (#606), so these are dups taken before it runs, and they live exactly as
  // long as the import they mirror -- same key, retired together. Without them
  // GetDmabuf has nothing to offer on this path, and every platform view is
  // composited however many planes are free.
  struct ScanoutBuffer {
    int fd[4]{-1, -1, -1, -1};
    uint32_t offset[4]{};
    uint32_t stride[4]{};
    uint32_t plane_count{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t fourcc{0};
    uint64_t modifier{0};
  };
  std::map<uint32_t, ScanoutBuffer> scanout;
  // Key of the buffer `current` points at, so its scanout twin can be found.
  uint32_t current_buffer_id{0};
  bool current_buffer_valid{false};
#endif

  // Common retire clock for both import paths (incremented on every submit).
  uint64_t submit_seq{0};

  // Ids the producer retired (ihs_pv_retire_buffer) while they were on screen;
  // each is dropped when a later frame supersedes it. Shared by both import
  // paths; mutable because the EGL path drains it from GetGlTextureName.
  // Guarded by `mutex`.
  mutable DeferredRetireSet deferred_retire;

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

  // The direct-scanout path's view of one layer's frames. Each layer that can
  // reach a plane has one: layer 0 below, and each extra layer its own.
  struct ScanoutOffer {
    // Deliver-once guard: the submit_seq last handed to the plane path. The
    // compositor commits faster than a 30fps producer submits, so a reused
    // overlay layer polls every present; without this it would re-import the
    // same buffer each time, churning AddFB2/retire for no new content. Set
    // when the compositor confirms the frame reached a plane
    // (AckDmabufScanout) or hands its slot back (OnScanoutRelease); a fresh
    // submit bumps submit_seq past it, re-arming delivery.
    uint64_t delivered_seq{0};
    // The submit_seq last handed out, which is not yet the same as delivered:
    // the compositor's import can fail after the hand-off, and a frame that
    // never reached a plane has to be offered again or a producer that does
    // not submit again never returns to scanout. Committed into delivered_seq
    // by AckDmabufScanout (placed) or OnScanoutRelease (given back). Zero when
    // nothing is outstanding.
    uint64_t offered_seq{0};
    // buffer_id of that outstanding offer, so a release for some other slot
    // (a stale retire) does not retire the offer.
    uint32_t offered_buffer_id{0};
  };
  mutable ScanoutOffer offer0;
  // Layer 0's layer_id (ihs_pv_submit_layers), which names its plane.
  uint32_t layer0_id{0};

  // Generation of each retired buffer_id (see
  // ICompositorSurface::Dmabuf::generation); an id missing here is at 0.
  // Bounded, since a producer may never reuse an id and so retire an endless
  // run of them. Forgetting an entry puts its id back at generation 0, which
  // can only cost a reuse of that id a held frame on the plane path while the
  // old framebuffer drains. Guarded by `mutex`.
  std::map<uint32_t, uint32_t> scanout_generation;
  static constexpr size_t kMaxScanoutGenerations = 1024;

  // Generation of @p buffer_id's current dma-buf. Caller holds `mutex`.
  [[nodiscard]] uint32_t GenerationLocked(const uint32_t buffer_id) const {
    const auto it = scanout_generation.find(buffer_id);
    return it == scanout_generation.end() ? 0 : it->second;
  }

  // Move @p buffer_id to its next generation. Caller holds `mutex`.
  void BumpGenerationLocked(const uint32_t buffer_id) {
    if (scanout_generation.size() >= kMaxScanoutGenerations &&
        scanout_generation.count(buffer_id) == 0) {
      scanout_generation.erase(scanout_generation.begin());
    }
    ++scanout_generation[buffer_id];
  }
  // Scanout keys of retired buffers, for TakeRetiredScanoutKeys. Bounded: on a
  // backend with no plane path nothing drains it. Guarded by `mutex`.
  std::vector<std::uintptr_t> retired_scanout_keys;
  static constexpr size_t kMaxRetiredScanoutKeys = 256;

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
    // submit_seq when this frame was stashed. Compared against the layer's
    // ScanoutOffer::delivered_seq to tell a frame the DRM scene path took (its
    // release rides OnScanoutRelease) from one superseded before it was ever
    // scanned out (its release eventfd must be signaled at supersede instead).
    uint64_t stash_seq{0};
    // The producer retired this frame's buffer_id and then submitted it again,
    // possibly for a different dma-buf, so a cached import under the id is
    // stale and must not be reused.
    bool reimport{false};
    // Where the frame lands (ihs_pv_submit_layers); applied at import, so the
    // geometry and the pixels it describes switch together.
    ICompositorSurface::LayerGeometry geom;
    // Owned copy of the frame's HDR metadata: frame.hdr points at plugin memory
    // we must not retain, so its contents are copied here at submit and
    // frame.hdr nulled. has_hdr is false for SDR frames.
    bool has_hdr{false};
    IhsHdrMetadata hdr{};
  };
  mutable PendingEglFrame pending_egl;
  mutable std::map<uint32_t, EglDmabufImporter::ImportedTexture> buffers_egl;
  mutable EglDmabufImporter::ImportedTexture* current_egl{nullptr};
  // Producer buffer_id of the frame current_egl was imported from, for
  // GetGlTextureBufferId. The GL path has no scanout retire to key a release
  // off, so the compositor tells one bound frame from the next by this.
  mutable uint32_t current_egl_buffer_id{0};
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

  // ---- Layers (ihs_pv_submit_layers) --------------------------------------
  //
  // Layer 0 is the frame the single-frame state above holds: an ihs_pv_submit,
  // or the bottom layer of a list. geom0 is the geometry of the frame layer 0
  // is sampling -- set with `current` at submit on Vulkan, and on EGL carried
  // on pending_egl and applied at import -- so the two never draw mismatched.
  mutable ICompositorSurface::LayerGeometry geom0;
  // A submit of 0 layers: nothing is drawn until the next submit.
  bool layer0_hidden{false};

  // The layers above the bottom one, each with its own frame, acquire fence,
  // release and plane offer; imports are shared with layer 0 through the
  // buffer_id-keyed cache. Keyed
  // by layer_id so a layer keeps its state while the list around it changes,
  // and a map because GetLayerGlTexture drops the lock for a fence wait: a
  // concurrent submit can then add or remove layers, and a node must not move
  // underneath it.
  struct ExtraLayer {
    ICompositorSurface::LayerGeometry geom;  // of the frame being sampled
    ScanoutOffer offer;
#if IVI_HAVE_VULKAN
    DmabufVulkanImporter::ImportedImage* current{nullptr};
    uint32_t current_buffer_id{0};
    uint32_t layout{VK_IMAGE_LAYOUT_UNDEFINED};
    int acquire_fd{-1};  // taken by the compositor, like pending_acquire_fd
#endif
#if IVI_HAVE_EGL
    PendingEglFrame pending;
    EglDmabufImporter::ImportedTexture* current_egl{nullptr};
    uint32_t current_egl_buffer_id{0};
#endif
  };
  mutable std::map<uint32_t, ExtraLayer> extra_layers;
  std::vector<uint32_t> extra_order;  // layer_ids, bottom to top

  // The extra layer drawn at index @p index (1-based over layer 0), or null.
  // Caller holds `mutex`.
  [[nodiscard]] ExtraLayer* ExtraAtLocked(const size_t index) const {
    if (index == 0 || index > extra_order.size()) {
      return nullptr;
    }
    const auto it = extra_layers.find(extra_order[index - 1]);
    return it == extra_layers.end() ? nullptr : &it->second;
  }

  // Drop every extra layer: a submit of a new list without them, a plain
  // ihs_pv_submit, or 0 layers. Caller holds `mutex`.
  void ClearExtraLayersLocked();

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

  // The compositor reports the KMS plane this view scanned out on this present
  // (0 == GL-composited). Stored for the DRM_PLANE grant accessor. Compositor
  // thread; the field is atomic.
  void SetScanoutPlane(uint32_t plane_id) override {
    drm_plane_id.store(plane_id, std::memory_order_relaxed);
  }

#if IVI_HAVE_EGL
  // Persisted HDR metadata (from the last submit), so the compositor holds the
  // connector's HDR_OUTPUT_METADATA steady while this view is on screen even on
  // presents with no fresh frame. Compositor thread; guarded like the frame.
  // EGL-only: the persisted HDR rides pending_egl, and the DRM scene path (the
  // sole consumer) is EGL-backed. In Vulkan-only builds the base
  // ICompositorSurface::GetHdrMetadata default (false) applies.
  [[nodiscard]] bool GetHdrMetadata(
      ICompositorSurface::Dmabuf::HdrMetadata* out) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!pending_egl.valid || !pending_egl.has_hdr) {
      return false;
    }
    const IhsHdrMetadata& h = pending_egl.hdr;
    out->transfer = h.transfer;
    for (int i = 0; i < 3; ++i) {
      out->display_primaries_x[i] = h.display_primaries_x[i];
      out->display_primaries_y[i] = h.display_primaries_y[i];
    }
    out->white_point_x = h.white_point_x;
    out->white_point_y = h.white_point_y;
    out->max_display_mastering_luminance = h.max_display_mastering_luminance;
    out->min_display_mastering_luminance = h.min_display_mastering_luminance;
    out->max_content_light_level = h.max_content_light_level;
    out->max_frame_average_light_level = h.max_frame_average_light_level;
    return true;
  }
#endif  // IVI_HAVE_EGL

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

#endif  // IVI_HAVE_EGL

  [[nodiscard]] size_t GetLayerCount() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return layer0_hidden ? 0 : 1 + extra_order.size();
  }

#if IVI_HAVE_VULKAN
  [[nodiscard]] VulkanLayerImage GetLayerVulkanImage(
      const size_t index) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    VulkanLayerImage out;
    const DmabufVulkanImporter::ImportedImage* img = nullptr;
    if (index == 0) {
      if (layer0_hidden) {
        return out;
      }
      img = current;
      out.geometry = geom0;
    } else if (const ExtraLayer* x = ExtraAtLocked(index); x != nullptr) {
      img = x->current;
      out.geometry = x->geom;
    }
    if (img == nullptr || img->image == VK_NULL_HANDLE) {
      return out;
    }
    out.image = reinterpret_cast<void*>(img->image);
    out.width = static_cast<int32_t>(img->width);
    out.height = static_cast<int32_t>(img->height);
    out.format = static_cast<uint32_t>(img->format);
    out.ycbcr_model = img->yuv ? static_cast<uint32_t>(img->ycbcr_model) : 0;
    out.ycbcr_range = img->yuv ? static_cast<uint32_t>(img->ycbcr_range) : 0;
    return out;
  }
  [[nodiscard]] uint32_t GetLayerVulkanImageLayout(
      const size_t index) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (index == 0) {
      return current_layout;
    }
    const ExtraLayer* x = ExtraAtLocked(index);
    return x != nullptr ? x->layout : 0;
  }
  void SetLayerVulkanImageLayout(const size_t index,
                                 const uint32_t layout) override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (index == 0) {
      current_layout = layout;
    } else if (ExtraLayer* x = ExtraAtLocked(index); x != nullptr) {
      x->layout = layout;
    }
  }
  [[nodiscard]] int TakeLayerAcquireFenceFd(const size_t index) override {
    const std::lock_guard<std::mutex> lock(mutex);
    int* slot = nullptr;
    if (index == 0) {
      slot = &pending_acquire_fd;
    } else if (ExtraLayer* x = ExtraAtLocked(index); x != nullptr) {
      slot = &x->acquire_fd;
    }
    if (slot == nullptr) {
      return -1;
    }
    const int fd = *slot;
    *slot = -1;
    return fd;
  }
#endif  // IVI_HAVE_VULKAN

#if IVI_HAVE_EGL
  // Defined out of line: it imports through g_egl_importer.
  [[nodiscard]] GlLayerTexture GetLayerGlTexture(size_t index) const override;

  // Where the frame a layer is about to import lives, re-found after every
  // time the lock was dropped (see ImportPendingEglLocked).
  struct EglSlot {
    PendingEglFrame* pending{nullptr};
    EglDmabufImporter::ImportedTexture** current{nullptr};
    uint32_t* current_id{nullptr};
    ICompositorSurface::LayerGeometry* geom{nullptr};
  };
  // Import the frame a layer has waiting, if any, and make it the layer's
  // current texture: wait out its acquire fence, reuse or create the import,
  // and apply its geometry. @p resolve fills an EglSlot for the layer and
  // returns false when the layer is gone; it is called again whenever the lock
  // has been dropped, because a concurrent submit may have replaced or removed
  // the layer meanwhile. Raster thread, GL context current; caller holds
  // @p lock.
  template <typename Resolve>
  void ImportPendingEglLocked(std::unique_lock<std::mutex>& lock,
                              const Resolve& resolve) const;
#endif  // IVI_HAVE_EGL

// The direct-scanout seam is not EGL's alone: a Vulkan backend places the same
// producer buffers on the same planes. Guarding it on IVI_HAVE_EGL left a
// Vulkan-only build with the base class's "cannot scan out", so every platform
// view composited however many planes were free.
#if IVI_HAVE_VULKAN || IVI_HAVE_EGL
  // Direct-scanout seam for the DRM compositor: expose a layer's latest
  // submitted frame as a dma-buf so it can be placed on a KMS plane instead of
  // being composited. Hands back *dups* of the frame's fds, owned by the
  // caller: duping under the lock keeps them valid even if a concurrent submit
  // supersedes and closes the originals before the compositor imports them.
  // The compositor plane-routes XOR composites a given surface per present, so
  // the two paths don't both consume a frame. Producers submit top-down
  // pixels (no REFLECT_Y), so the plane scans the buffer out as-is.
  //
  // On EGL a composite of a frame imports it (GetGlTextureName), which
  // consumes it, so the frame is not offered to a plane after that until the
  // producer submits again. A continuous producer resumes direct scanout on
  // its next submit; a static one stays composited (still correct, just not
  // zero-copy) until it resubmits.

  // One layer, as the offer path reads it. Caller holds `mutex`.
  struct LayerRef {
    ScanoutOffer* offer{nullptr};
    uint32_t layer_id{0};
    uint32_t buffer_id{0};
    // A frame submitted and not yet taken by the plane path.
    bool fresh{false};
    ICompositorSurface::LayerGeometry geom;
    uint32_t width{0};
    uint32_t height{0};
    int acquire_fd{-1};  // borrowed; an offer hands out a dup
#if IVI_HAVE_VULKAN
    bool vulkan{false};  // imported at submit, scanout dups in `scanout`
#endif
#if IVI_HAVE_EGL
    const PendingEglFrame* pending{nullptr};  // the stashed frame on offer
#endif
  };

  // Resolve layer @p index (0 = the bottom). False when there is no such layer
  // or nothing has been submitted for it. Caller holds `mutex`.
  bool ResolveLayerLocked(const size_t index, LayerRef* ref) const {
    if (layer0_hidden) {
      return false;
    }
    ExtraLayer* x = nullptr;
    if (index == 0) {
      ref->offer = &offer0;
      ref->layer_id = layer0_id;
    } else {
      x = ExtraAtLocked(index);
      if (x == nullptr) {
        return false;
      }
      ref->offer = &x->offer;
      ref->layer_id = extra_order[index - 1];
    }
    const bool undelivered = ref->offer->delivered_seq != submit_seq;
#if IVI_HAVE_VULKAN
    const DmabufVulkanImporter::ImportedImage* img =
        x == nullptr ? current : x->current;
    if (img != nullptr) {
      ref->vulkan = true;
      ref->buffer_id = x == nullptr ? current_buffer_id : x->current_buffer_id;
      ref->geom = x == nullptr ? geom0 : x->geom;
      ref->width = img->width;
      ref->height = img->height;
      ref->acquire_fd = x == nullptr ? pending_acquire_fd : x->acquire_fd;
      ref->fresh = undelivered;
      return true;
    }
#endif
#if IVI_HAVE_EGL
    const PendingEglFrame& pending = x == nullptr ? pending_egl : x->pending;
    if (pending.valid) {
      ref->pending = &pending;
      ref->buffer_id = pending.frame.buffer_id;
      ref->geom = pending.geom;
      ref->width = pending.frame.width;
      ref->height = pending.frame.height;
      ref->acquire_fd = pending.acquire_fence_fd;
      ref->fresh = undelivered;
      return true;
    }
    // Already imported for a composite: placeable, but not offered again.
    const EglDmabufImporter::ImportedTexture* tex =
        x == nullptr ? current_egl : x->current_egl;
    if (tex != nullptr) {
      ref->buffer_id =
          x == nullptr ? current_egl_buffer_id : x->current_egl_buffer_id;
      ref->geom = x == nullptr ? geom0 : x->geom;
      ref->width = tex->width;
      ref->height = tex->height;
      return true;
    }
#endif
    (void)undelivered;
    return false;
  }

  // How many layers show @p id. Caller holds `mutex`.
  [[nodiscard]] size_t LayersShowingLocked(const uint32_t id) const {
    size_t n = 0;
    const size_t count = 1 + extra_order.size();
    for (size_t i = 0; i < count; ++i) {
      LayerRef r;
      if (ResolveLayerLocked(i, &r) && r.buffer_id == id) {
        ++n;
      }
    }
    return n;
  }

  // Hand @p ref's frame to the plane path, once. Caller holds `mutex`.
  DmabufState OfferLayerLocked(const LayerRef& ref, Dmabuf* out) const {
    if (!ref.fresh) {
      return DmabufState::kNoNewFrame;
    }
    if constexpr (!kScanoutKeyHasGeneration) {
      // No room in a key for the generation, so a reused id would reach the
      // plane under its old memory's key. Composite it instead.
      if (GenerationLocked(ref.buffer_id) != 0) {
        return DmabufState::kNotScanoutCapable;
      }
    }
#if IVI_HAVE_VULKAN
    // The frame was imported at submit and its fds consumed, so what a plane
    // can be given is the dup retained beside that import rather than a
    // stashed frame.
    if (ref.vulkan) {
      const auto sit = scanout.find(ref.buffer_id);
      if (sit == scanout.end() || sit->second.plane_count == 0) {
        // Imported but not dup'd -- a producer whose fds could not be
        // duplicated. New content exists and cannot be scanned out, which is
        // exactly what the composite fallback is for.
        return DmabufState::kNotScanoutCapable;
      }
      const ScanoutBuffer& sb = sit->second;
      int duped[4] = {-1, -1, -1, -1};
      for (uint32_t i = 0; i < sb.plane_count; ++i) {
        duped[i] = sb.fd[i] >= 0 ? ::dup(sb.fd[i]) : -1;
        if (duped[i] < 0) {
          for (uint32_t j = 0; j < i; ++j) {
            ::close(duped[j]);
          }
          return DmabufState::kNotScanoutCapable;
        }
      }
      *out = Dmabuf{};
      out->width = sb.width;
      out->height = sb.height;
      out->fourcc = sb.fourcc;
      out->modifier = sb.modifier;
      out->plane_count = sb.plane_count;
      for (uint32_t i = 0; i < sb.plane_count; ++i) {
        out->fd[i] = duped[i];
        out->offset[i] = sb.offset[i];
        out->stride[i] = sb.stride[i];
      }
      out->buffer_id = ref.buffer_id;
      out->generation = GenerationLocked(ref.buffer_id);
      // The acquire fence is the producer's, and the compositor lowers it to
      // the plane's IN_FENCE_FD. dup: the layer's copy stays ours, and the
      // blend path may still want it if this frame ends up composited.
      out->acquire_fence_fd = ref.acquire_fd >= 0 ? ::dup(ref.acquire_fd) : -1;
      ref.offer->offered_seq = submit_seq;
      ref.offer->offered_buffer_id = ref.buffer_id;
      return DmabufState::kFrame;
    }
#endif
#if IVI_HAVE_EGL
    if (ref.pending == nullptr) {
      return DmabufState::kNoNewFrame;
    }
    const IhsFrame& f = ref.pending->frame;
    // From here on there *is* a new frame, so every remaining bail-out is a
    // frame that cannot be scanned out rather than an absent one. Saying so
    // lets the compositor composite the new content instead of holding the
    // plane on the last scannable frame.
    if (f.plane_count == 0 || f.plane_fd[0] < 0) {
      return DmabufState::kNotScanoutCapable;
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
        // A plane without a handle can't be scanned out.
        return DmabufState::kNotScanoutCapable;
      }
    }
    int duped[4] = {-1, -1, -1, -1};
    for (uint32_t i = 0; i < np; ++i) {
      duped[i] = ::dup(f.plane_fd[i]);
      if (duped[i] < 0) {
        for (uint32_t j = 0; j < i; ++j) {
          ::close(duped[j]);
        }
        // Transient (fd exhaustion), not a property of the frame -- and the
        // frame is not marked delivered, so the next present retries the dup on
        // the same buffer. Report it as "no new frame" so the plane holds for
        // one present and self-heals, rather than dropping the whole frame to
        // GL over a failure that is likely gone by the next commit.
        return DmabufState::kNoNewFrame;
      }
    }
    *out = Dmabuf{};
    for (uint32_t i = 0; i < np; ++i) {
      out->fd[i] = duped[i];  // owned by the caller
    }
    out->fourcc = f.format.fourcc;
    out->modifier = f.format.modifier;
    out->color_space = f.color_space;  // YUV colorimetry for the plane CSC
    out->color_range = f.color_range;
    // HDR metadata is not carried per-frame on the Dmabuf: the DRM scene path
    // reads the view's persisted metadata via GetHdrMetadata (steady across
    // reuse presents) rather than the deliver-once frame.
    out->width = f.width;
    out->height = f.height;
    out->plane_count = np;
    out->buffer_id = f.buffer_id;  // the compositor hands it back on release
    out->generation = GenerationLocked(f.buffer_id);
    for (uint32_t i = 0; i < np; ++i) {
      out->offset[i] = f.plane_offset[i];
      out->stride[i] = f.plane_stride[i];
    }
    // Explicit sync: hand the producer's acquire fence to the DRM scene path,
    // which wires it to the plane's IN_FENCE_FD (or CPU-waits as a fallback).
    // Per the ICompositorSurface::Dmabuf contract the surface keeps ownership
    // and the compositor gets a dup; retaining it here also lets the GL-texture
    // fallback still wait on the fence if this present later falls back after
    // the offer. The surface's copy is closed on the next superseding submit,
    // on dispose, or by the fallback wait. -1 when the producer synced before
    // submit.
    out->acquire_fence_fd = -1;
    if (ref.acquire_fd >= 0) {
      out->acquire_fence_fd = ::dup(ref.acquire_fd);
      if (out->acquire_fence_fd < 0) {
        // Fall back to implicit sync for this frame; log so it's diagnosable.
        ihs::log::warn(
            "[ihs_pv] dup(acquire_fence) failed (errno={}); implicit sync "
            "this frame",
            errno);
      }
    }
    // An older offer still open, for a frame since superseded, was abandoned.
    AbandonStaleOfferLocked(*ref.offer, f.buffer_id);
    // Offered, not delivered. The compositor commits this by acking the frame
    // onto a plane, or retires it by handing the slot back; until one of those
    // the frame stays eligible, so an import that fails downstream can retry.
    ref.offer->offered_seq = submit_seq;
    ref.offer->offered_buffer_id = f.buffer_id;
    return DmabufState::kFrame;
#else
    return DmabufState::kNoNewFrame;
#endif
  }

  // The single-layer form: layer 0 drawn whole, or nothing. A plane takes one
  // buffer drawn across the whole view here, so a new frame of anything more
  // -- layers above the bottom one, or a bottom layer cropped, placed or
  // rotated -- is reported as not scanout-capable and the view is composited.
  [[nodiscard]] DmabufState GetDmabuf(Dmabuf* out) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    LayerRef ref;
    if (out == nullptr || !ResolveLayerLocked(0, &ref)) {
      return DmabufState::kNoNewFrame;
    }
    if (!extra_order.empty() || !ref.geom.IsWhole()) {
      return ref.fresh ? DmabufState::kNotScanoutCapable
                       : DmabufState::kNoNewFrame;
    }
    return OfferLayerLocked(ref, out);
  }

  [[nodiscard]] DmabufState GetLayerDmabuf(const size_t index,
                                           LayerDmabuf* out) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    LayerRef ref;
    if (out == nullptr || !ResolveLayerLocked(index, &ref)) {
      return DmabufState::kNoNewFrame;
    }
    out->layer_id = ref.layer_id;
    out->geometry = ref.geom;
    out->buffer_width = ref.width;
    out->buffer_height = ref.height;
    // One buffer in two layers has one release, and two planes would each
    // report it, the first while the second still scans it out.
    if (ref.fresh && LayersShowingLocked(ref.buffer_id) > 1) {
      return DmabufState::kNotScanoutCapable;
    }
    return OfferLayerLocked(ref, &out->dmabuf);
  }

  [[nodiscard]] std::vector<std::uintptr_t> TakeRetiredScanoutKeys() override {
    const std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::uintptr_t> out;
    out.swap(retired_scanout_keys);
    return out;
  }
#endif  // IVI_HAVE_VULKAN || IVI_HAVE_EGL

#if IVI_HAVE_EGL
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

  // Producer buffer_id of the frame currently bound as a GL texture. Raster
  // thread, same as GetGlTextureName.
  //
  // Guarded: current_egl only exists under IVI_HAVE_EGL, and this accessor sits
  // outside that block because it is part of the generic surface interface. A
  // Vulkan-only build has no GL texture to name, so 0 is the honest answer --
  // the compositor's GL-composite path that consumes this is not built there
  // either.
  [[nodiscard]] uint32_t GetGlTextureBufferId() const override {
#if IVI_HAVE_EGL
    const std::lock_guard<std::mutex> lock(mutex);
    return current_egl != nullptr ? current_egl_buffer_id : 0;
#else
    return 0;
#endif
  }

  // The DRM scene path retired this frame; wake the producer's release fence
  // for its ring slot. Compositor thread.
  void OnScanoutRelease(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mutex);
    // Terminal for an outstanding offer: the slot goes back to the producer,
    // which may overwrite it, so the frame must not be offered again.
    RetireOffer(buffer_id);
    SignalRelease(buffer_id);
  }

  // The plane path's release, naming the generation that left the plane. A
  // release for an older generation of a reused id is for memory the producer
  // already retired -- the retire answered its release -- so it must not hand
  // back the id's current frame. Compositor thread.
  void OnScanoutKeyRelease(const std::uintptr_t key) override {
    const std::lock_guard<std::mutex> lock(mutex);
    const uint32_t buffer_id = ScanoutKeyBufferId(key);
    if (key != ScanoutKey(buffer_id, GenerationLocked(buffer_id))) {
      return;
    }
    RetireOffer(buffer_id);
    SignalRelease(buffer_id);
  }

  // The compositor placed the offered frame on a plane. Commit the
  // deliver-once guard, which until now was only provisional. Compositor
  // thread.
  void AckDmabufScanout(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mutex);
    RetireOffer(buffer_id);
  }

  // Whether anything has been produced for this view yet: a frame waiting to
  // be taken, or one already imported for GL. Distinguishes a producer that
  // has not started from one that is merely idle. Any thread.
  [[nodiscard]] bool HasContent() const override {
    const std::lock_guard<std::mutex> lock(mutex);
#if IVI_HAVE_VULKAN
    if (current != nullptr && current->image != VK_NULL_HANDLE) {
      return true;
    }
#endif
#if IVI_HAVE_EGL
    if (pending_egl.valid || current_egl != nullptr) {
      return true;
    }
#endif
    return !extra_order.empty();
  }

  // Close out the outstanding offer for `buffer_id`, whether it was placed or
  // given back -- both mean the frame will not be offered again. Offers are
  // per layer, and a buffer is offered by at most one layer at a time (see
  // GetLayerDmabuf), so whichever layer holds it is the one. A release for
  // any other slot is a stale retire and leaves the offers alone. Caller holds
  // `mutex`.
  void RetireOffer(uint32_t buffer_id) const {
    const auto retire = [buffer_id](ScanoutOffer& o) {
      if (o.offered_seq == 0 || o.offered_buffer_id != buffer_id) {
        return;
      }
      o.delivered_seq = o.offered_seq;
      o.offered_seq = 0;
      o.offered_buffer_id = 0;
    };
    retire(offer0);
    for (auto& [layer_id, x] : extra_layers) {
      retire(x.offer);
    }
  }

  // Answer an offer the compositor took and never answered -- neither placed
  // nor released -- once a different frame is going to screen instead. That
  // happens when a present drops its frame (GL fallback on a secondary
  // output), a GL import fails, or GL fallback is latched and nothing asks for
  // the dma-buf any more. Deliberately leaving an offer open to have it
  // re-offered (a pool swap that failed) re-offers the same buffer, which
  // @p keep_id spares.
  //
  // A superseding submit cannot release an open offer's frame itself: the
  // compositor may be importing it at that moment. By the time the compositor
  // offers or imports something newer for the same layer it has finished with
  // the old offer, so this is the first point the release is safe -- and
  // without it the producer waits out its release timeout on that buffer.
  // Caller holds `mutex`.
  void AbandonStaleOfferLocked(ScanoutOffer& offer,
                               const uint32_t keep_id) const {
    if (offer.offered_seq == 0 || offer.offered_buffer_id == keep_id) {
      return;
    }
    const uint32_t id = offer.offered_buffer_id;
    offer.delivered_seq = offer.offered_seq;
    offer.offered_seq = 0;
    offer.offered_buffer_id = 0;
    SignalRelease(id);
  }

#if IVI_HAVE_EGL
  // Whether a stashed frame being superseded (or dropped with its layer) is
  // this side's to release. Not when it is on a plane, where the plane path's
  // release will do it, and not while an offer of it is open: the compositor
  // owes an ack or a release either way, and releasing here would hand the
  // slot back to the producer mid-import, with the producer by definition
  // writing a new frame right now.
  //
  // `delivered_seq` moves only when the compositor acks the frame onto a
  // plane or hands its slot back, so it does mean "reached a plane" (#332).
  // The `!on_a_plane` arm is the belt-and-braces half: a compositor that
  // takes a frame and answers neither would otherwise leave the producer
  // waiting out an eventfd that never fires. SignalRelease is idempotent, so
  // releasing a slot twice costs nothing. drm_plane_id is 0 exactly when the
  // last present composited this view (SetScanoutPlane(0)), which is the case
  // where no retire is coming. Caller holds `mutex`.
  [[nodiscard]] bool OwnsSupersededReleaseLocked(
      const ScanoutOffer& offer,
      const PendingEglFrame& pending) const {
    const bool on_a_plane = drm_plane_id.load(std::memory_order_relaxed) != 0;
    const bool offer_outstanding =
        offer.offered_seq != 0 && offer.offered_seq >= pending.stash_seq;
    return !offer_outstanding &&
           (offer.delivered_seq < pending.stash_seq || !on_a_plane);
  }
#endif

  // Signal and drop the release eventfd for `buffer_id` (a no-op if there is
  // none). Writing wakes the producer's poll on its dup; closing our copy
  // retires it. Caller holds `mutex`.
  void SignalRelease(uint32_t buffer_id) const {
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

#if IVI_HAVE_EGL
  // Drop the GL import cached for `id` (it is destroyed on the raster thread
  // once the reap margin has passed) and wake and forget the slot's release
  // eventfd. Caller holds `mutex`.
  void RetireEglImportLocked(uint32_t id) const;
  // Drop the GL import cached for `id` without touching its release eventfd:
  // for an import replaced by a newer frame under the same id (a resize, or a
  // re-used retired id), whose eventfd already belongs to that newer frame.
  // Caller holds `mutex`.
  void DropEglImportLocked(uint32_t id) const;

  // True while `id` is the frame waiting to be imported or the one being
  // sampled. Caller holds `mutex`.
  [[nodiscard]] bool OnScreenEglLocked(const uint32_t id) const {
    if ((pending_egl.valid && pending_egl.frame.buffer_id == id) ||
        (current_egl != nullptr && current_egl_buffer_id == id)) {
      return true;
    }
    return std::any_of(
        extra_layers.begin(), extra_layers.end(), [id](const auto& entry) {
          const ExtraLayer& x = entry.second;
          return (x.pending.valid && x.pending.frame.buffer_id == id) ||
                 (x.current_egl != nullptr && x.current_egl_buffer_id == id);
        });
  }
#endif

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
      // The caller may already have stored a dup of the compositor's release
      // fence here (HandBackReleaseFence runs first on the EGL path). The
      // eventfd supersedes it; overwriting without closing leaked one sync_file
      // per explicit-sync submit, which grew the fd table through each power
      // of two (a ~100 ms RCU stall on a Pi 4 at 256 and 512) and reached the
      // 1024 soft limit about half a minute into a 30fps stream.
      if (*out_fd >= 0) {
        close(*out_fd);
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

// Margin, in submits, before a retired import is destroyed: safely more than
// the frames the compositor keeps in flight, so no present command buffer still
// binds it by the time it is freed.
constexpr uint64_t kImportRetireMargin = 8;

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
  // Extra layers first: dropping one signals its slot through release_efds.
  ClearExtraLayersLocked();
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
  // The scanout dups are plain fds, so they close here rather than going
  // through the deferred free the VkImages need.
  for (auto& [id, sb] : scanout) {
    for (int& fd : sb.fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }
  scanout.clear();
  current_buffer_valid = false;
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

#if IVI_HAVE_VULKAN
// Close the dups retained for direct scanout. Safe while a plane is still
// scanning the buffer: the KMS framebuffer holds its own reference, and the
// scene's pool dups whatever it keeps.
void CloseScanoutBuffer(IhsPluginView::ScanoutBuffer* sb) {
  if (sb == nullptr) {
    return;
  }
  for (int& fd : sb->fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  sb->plane_count = 0;
}

// Dup @p frame's plane fds for direct scanout, before an import consumes
// them. False, with nothing left open, when a plane has no fd or a dup fails;
// the frame is then composited rather than placed, which is not fatal.
bool DupForScanout(const IhsFrame& frame, IhsPluginView::ScanoutBuffer* sb) {
  *sb = {};
  sb->plane_count = frame.plane_count < 4 ? frame.plane_count : 4;
  sb->width = frame.width;
  sb->height = frame.height;
  sb->fourcc = frame.format.fourcc;
  sb->modifier = frame.format.modifier;
  bool ok = sb->plane_count > 0;
  for (uint32_t pi = 0; pi < sb->plane_count; ++pi) {
    sb->offset[pi] = frame.plane_offset[pi];
    sb->stride[pi] = frame.plane_stride[pi];
    sb->fd[pi] = frame.plane_fd[pi] >= 0 ? ::dup(frame.plane_fd[pi]) : -1;
    if (sb->fd[pi] < 0) {
      ok = false;
    }
  }
  if (!ok) {
    CloseScanoutBuffer(sb);
  }
  return ok;
}

// True while `id` is the frame the compositor samples. Caller holds v->mutex.
bool OnScreenVulkanLocked(const IhsPluginView* v, const uint32_t id) {
  if (v->current != nullptr && v->current_buffer_id == id) {
    return true;
  }
  return std::any_of(v->extra_layers.begin(), v->extra_layers.end(),
                     [id](const auto& entry) {
                       return entry.second.current != nullptr &&
                              entry.second.current_buffer_id == id;
                     });
}

// Drop the Vulkan import cached for `id` (destroyed on a later submit once the
// reap margin has passed) and the scanout dups beside it. Caller holds
// v->mutex.
void RetireVulkanImportLocked(IhsPluginView* v, const uint32_t id) {
  if (const auto it = v->buffers.find(id); it != v->buffers.end()) {
    if (v->current == &it->second) {
      v->current = nullptr;
      v->current_buffer_valid = false;
    }
    // An extra layer showing the same buffer loses it too, rather than keep a
    // pointer into an erased node.
    for (auto& [layer_id, x] : v->extra_layers) {
      if (x.current == &it->second) {
        x.current = nullptr;
      }
    }
    v->retired.push_back({it->second, v->submit_seq + kImportRetireMargin});
    v->buffers.erase(it);
  }
  if (const auto sit = v->scanout.find(id); sit != v->scanout.end()) {
    CloseScanoutBuffer(&sit->second);
    v->scanout.erase(sit);
  }
}
#endif

#if IVI_HAVE_EGL
void IhsPluginView::DropEglImportLocked(const uint32_t id) const {
  if (const auto it = buffers_egl.find(id); it != buffers_egl.end()) {
    if (current_egl == &it->second) {
      current_egl = nullptr;
    }
    for (auto& [layer_id, x] : extra_layers) {
      if (x.current_egl == &it->second) {
        x.current_egl = nullptr;
      }
    }
    retired_egl.push_back({it->second, submit_seq + kImportRetireMargin});
    buffers_egl.erase(it);
  }
}

void IhsPluginView::RetireEglImportLocked(const uint32_t id) const {
  DropEglImportLocked(id);
  SignalRelease(id);
}
#endif

void IhsPluginView::ClearExtraLayersLocked() {
  for (auto& [layer_id, x] : extra_layers) {
#if IVI_HAVE_VULKAN
    if (x.acquire_fd >= 0) {
      close(x.acquire_fd);
    }
#endif
#if IVI_HAVE_EGL
    if (x.pending.valid) {
      // Never drawn: unless a plane has it, nothing else will release its
      // slot.
      if (OwnsSupersededReleaseLocked(x.offer, x.pending)) {
        SignalRelease(x.pending.frame.buffer_id);
      }
      CloseFrameFds(&x.pending.frame);
      if (x.pending.acquire_fence_fd >= 0) {
        close(x.pending.acquire_fence_fd);
      }
    }
#endif
    (void)x;
  }
  extra_layers.clear();
  extra_order.clear();
}

#if IVI_HAVE_EGL
// Raster-thread lazy import for the EGL path: HostSubmit stashed the frame
// (plugin thread, no GL context); import it here, where the EGL compositor
// calls us with the context current, caching per ring buffer like the Vulkan
// path.
template <typename Resolve>
void IhsPluginView::ImportPendingEglLocked(std::unique_lock<std::mutex>& lock,
                                           const Resolve& resolve) const {
  EglSlot slot;
  if (!resolve(&slot) || !slot.pending->valid) {
    return;
  }
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
  while (slot.pending->valid && slot.pending->acquire_fence_fd >= 0) {
    const int fence = slot.pending->acquire_fence_fd;
    slot.pending->acquire_fence_fd = -1;
    // Explicit sync (#513): hand the fence to the GL driver and let the GPU
    // wait on it. Cheap and non-blocking, so it runs under the lock — unlike
    // the CPU wait below, there is nothing to drop the lock across. On
    // success EGL owns the fd, so we must not close it here.
    if (g_egl_importer.WaitAcquireFence(fence)) {
      continue;
    }
    // No native fence sync on this display (or the sync failed): fall back to
    // the CPU wait, which still owns the fd.
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
    // The view may have been disposed, or this layer replaced or removed,
    // while the lock was dropped for the wait.
    if (!resolve(&slot)) {
      return;
    }
  }
  if (!slot.pending->valid) {
    return;
  }
  // The compositor samples the texture synchronously after this, so GL's own
  // deferred deletion already covers in-flight draws; the reap margin is
  // safety.
  for (auto rit = retired_egl.begin(); rit != retired_egl.end();) {
    if (submit_seq >= rit->reap_at) {
      g_egl_importer.Destroy(&rit->texture);  // GL context current here
      rit = retired_egl.erase(rit);
    } else {
      ++rit;
    }
  }
  IhsFrame& f = slot.pending->frame;
  if (slot.pending->reimport) {
    // Not RetireEglImportLocked: the id's release eventfd is this new frame's.
    DropEglImportLocked(f.buffer_id);
    slot.pending->reimport = false;
  }
  const auto it = buffers_egl.find(f.buffer_id);
  if (it != buffers_egl.end() && it->second.width == f.width &&
      it->second.height == f.height) {
    CloseFrameFds(&f);  // redundant handle to the cached import
    *slot.current = &it->second;
    *slot.current_id = f.buffer_id;
  } else {
    if (it != buffers_egl.end()) {
      // A resize: the old import goes, and every layer still pointing at it
      // is cleared. Its release eventfd is this new frame's, so no release.
      DropEglImportLocked(f.buffer_id);
    }
    EglDmabufImporter::ImportedTexture imported;
    if (g_egl_importer.Import(f, &imported)) {
      auto [pos, ins] = buffers_egl.emplace(f.buffer_id, imported);
      *slot.current = &pos->second;
      *slot.current_id = f.buffer_id;
      // Synthesised ids never repeat, so every earlier entry is dead. Retire
      // them (the reap margin covers a compositor present still binding one)
      // so the cache holds just the current import rather than growing per
      // frame. Only a plain ihs_pv_submit producer synthesises ids, and such
      // a submit clears the extra layers, so no other layer holds one.
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
  *slot.geom = slot.pending->geom;
  slot.pending->valid = false;
  // A retired frame that was on screen may be off it now.
  for (const uint32_t id : deferred_retire.ReleaseOffScreen(
           [this](uint32_t id) { return OnScreenEglLocked(id); })) {
    RetireEglImportLocked(id);
  }
}

uint32_t IhsPluginView::GetGlTextureName() const {
  std::unique_lock<std::mutex> lock(mutex);
  // Importing a newer frame for layer 0 means any open offer of an older one
  // was abandoned.
  if (pending_egl.valid) {
    AbandonStaleOfferLocked(offer0, pending_egl.frame.buffer_id);
  }
  ImportPendingEglLocked(lock, [this](EglSlot* s) {
    s->pending = &pending_egl;
    s->current = &current_egl;
    s->current_id = &current_egl_buffer_id;
    s->geom = &geom0;
    return true;
  });
  return current_egl != nullptr ? current_egl->texture : 0;
}

ICompositorSurface::GlLayerTexture IhsPluginView::GetLayerGlTexture(
    const size_t index) const {
  GlLayerTexture out;
  if (index == 0) {
    // Layer 0 is the single-frame path; GetGlTextureName imports its frame.
    out.name = GetGlTextureName();
    const std::lock_guard<std::mutex> lock(mutex);
    if (layer0_hidden || out.name == 0 || current_egl == nullptr) {
      return {};
    }
    out.width = static_cast<int32_t>(current_egl->width);
    out.height = static_cast<int32_t>(current_egl->height);
    out.external = current_egl->external;
    out.top_first = true;  // imported dma-bufs are top-first
    out.buffer_id = current_egl_buffer_id;
    out.geometry = geom0;
    return out;
  }
  std::unique_lock<std::mutex> lock(mutex);
  if (index > extra_order.size()) {
    return out;
  }
  const uint32_t layer_id = extra_order[index - 1];
  if (const auto it = extra_layers.find(layer_id);
      it != extra_layers.end() && it->second.pending.valid) {
    AbandonStaleOfferLocked(it->second.offer,
                            it->second.pending.frame.buffer_id);
  }
  ImportPendingEglLocked(lock, [this, layer_id](EglSlot* s) {
    const auto it = extra_layers.find(layer_id);
    if (it == extra_layers.end()) {
      return false;
    }
    s->pending = &it->second.pending;
    s->current = &it->second.current_egl;
    s->current_id = &it->second.current_egl_buffer_id;
    s->geom = &it->second.geom;
    return true;
  });
  const auto it = extra_layers.find(layer_id);
  if (it == extra_layers.end() || it->second.current_egl == nullptr) {
    return out;
  }
  const ExtraLayer& x = it->second;
  out.name = x.current_egl->texture;
  out.width = static_cast<int32_t>(x.current_egl->width);
  out.height = static_cast<int32_t>(x.current_egl->height);
  out.external = x.current_egl->external;
  out.top_first = true;
  out.buffer_id = x.current_egl_buffer_id;
  out.geometry = x.geom;
  return out;
}
#endif

// platform_view_listener trampolines: the registry drives these with the
// listener context (the IhsPluginView*), which we forward into the plugin's
// IhsPvCallbacks.
//
// accept_gesture/reject_gesture are wired for completeness rather than because
// anything calls them. Flutter emits acceptGesture/rejectGesture only from
// DarwinPlatformViewController; the PlatformViewLink/PlatformViewSurface path
// an ihs_pv view is built from settles the gesture arena inside the render
// object, flushing or dropping the cached pointer events without a platform
// message. So a plugin's callbacks stay unreached on Linux until something
// drives that channel — the host side is simply no longer the reason why.
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

void ListenerAcceptGesture(int32_t /* id */, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.accept_gesture != nullptr) {
    view->callbacks.accept_gesture(view->plugin_user_data);
  }
}

void ListenerRejectGesture(int32_t /* id */, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.reject_gesture != nullptr) {
    view->callbacks.reject_gesture(view->plugin_user_data);
  }
}

void ListenerSetSuspended(int32_t /* id */, uint8_t suspended, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.set_suspended != nullptr) {
    view->callbacks.set_suspended(view->plugin_user_data, suspended);
  }
}

void ListenerRenegotiate(int32_t /* id */, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.renegotiate != nullptr) {
    view->callbacks.renegotiate(view->plugin_user_data);
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
    /* accept_gesture */ ListenerAcceptGesture,
    /* reject_gesture */ ListenerRejectGesture,
    /* set_suspended */ ListenerSetSuspended,
    /* renegotiate */ ListenerRenegotiate,
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

// The formats the dma-buf kinds offer when the active importer can say which
// modifiers it will take: those first, best first, then the assumed list
// behind them.
//
// Preference, not a filter. A producer that can honor the order lands on a
// modifier the driver admits; one that can only make LINEAR -- anything
// filling pixels on the CPU -- still finds it further down the list and keeps
// working. Dropping the assumed entries would impose tiling on every such
// producer to satisfy a rule nothing currently enforces. A duplicate is
// harmless: it is the same offer twice, and the first wins.
//
// Empty when the probe found nothing, which leaves the assumed list to stand
// on its own.
template <typename Probe>
std::vector<IhsFormatModifier> ProbedOffer(const char* importer,
                                           const Probe& probe) {
  std::vector<IhsFormatModifier> offered;
  for (const IhsFormatModifier& f : kDmabufImportFormats) {
    for (const uint64_t m : probe(f.fourcc)) {
      offered.push_back({f.fourcc, 0, m});
    }
  }
  if (offered.empty()) {
    return offered;
  }
  for (const IhsFormatModifier& f : kDmabufImportFormats) {
    offered.push_back(f);
  }
  ihs::log::debug("[ihs_pv] dma-buf formats ({}): {} device-probed, {} assumed",
                  importer, offered.size() - std::size(kDmabufImportFormats),
                  std::size(kDmabufImportFormats));
  return offered;
}

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
      // Explicit-sync acquire on EGL: the GL-composite path waits on the
      // producer's sync_file with eglWaitSyncKHR before sampling the import
      // (GetGlTextureName), which needs EGL_ANDROID_native_fence_sync on the
      // backend's display. The importer probed that at host install, so ask it
      // rather than re-querying here. Advertise it only when the wait is
      // actually wired: ihs_pv_negotiate refuses EXPLICIT_REQUIRED and caps the
      // granted sync on this flag, so claiming it without honoring it hands the
      // producer a fence nothing waits on (#513).
      if (g_egl_importer.has_native_fence_sync()) {
        out->explicit_sync = 1;
      }
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
#if IVI_HAVE_VULKAN
    // Offer what the device says it will import before what we assume it will
    // (#597). The hardcoded list is LINEAR for every fourcc, and at least one
    // driver does not advertise LINEAR as sampleable -- it imports the frame
    // and samples it anyway, which is invalid however well it works.
    static std::vector<IhsFormatModifier> offered_vk;
    static std::once_flag probed_vk;
    std::call_once(probed_vk, [] {
      if (g_importer.ready()) {
        offered_vk = ProbedOffer("vulkan", [](uint32_t fourcc) {
          return g_importer.ImportableModifiers(fourcc);
        });
      }
    });
    if (!offered_vk.empty()) {
      out->formats = offered_vk.data();
      out->format_count = offered_vk.size();
      return IHS_PV_OK;
    }
#endif
#if IVI_HAVE_EGL
    // The same for an EGL backend. Without it the offer is LINEAR only, so a
    // producer that could hand the compositor a tiled or compressed buffer
    // (AFBC on Mali, UBWC on Adreno) is steered onto the layout that costs
    // the most bandwidth to sample and to scan out.
    static std::vector<IhsFormatModifier> offered_egl;
    static std::once_flag probed_egl;
    std::call_once(probed_egl, [] {
      if (g_egl_importer.ready()) {
        offered_egl = ProbedOffer("egl", [](uint32_t fourcc) {
          return g_egl_importer.ImportableModifiers(fourcc);
        });
      }
    });
    if (!offered_egl.empty()) {
      out->formats = offered_egl.data();
      out->format_count = offered_egl.size();
      return IHS_PV_OK;
    }
#endif
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
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  // Per the ABI, the accessor is 0 unless the current grant is DRM_PLANE.
  // Otherwise it is the plane the compositor scanned this view out on at the
  // last present -- and 0 while the view is GL-composited (no plane), so a
  // direct-scanout producer sees when its zero-GPU path is not being honored.
  if (v->granted_kind != IHS_PV_KIND_DRM_PLANE) {
    return 0;
  }
  return v->drm_plane_id.load(std::memory_order_relaxed);
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

// The rest of a layer list, applied under the same hold of the view lock that
// swaps in layer 0, so no present can draw one layer's new frame over another's
// old one. Its frames and fences are owned here until ApplyLayerListLocked
// takes them; a submit that fails before that leaves them to the caller.
struct ExtraSubmit {
  uint32_t layer_id{0};
  IhsFrame frame{};
  int acquire_fd{-1};
  int* out_release_fd{nullptr};
  ICompositorSurface::LayerGeometry geom;
};
struct LayerListUpdate {
  std::vector<ExtraSubmit> extras;
  bool applied{false};
};

// Copy a producer's frame into a full-size one. It may be built against an
// older, smaller IhsFrame: copying *in whole would read past the end of its
// object, and the trailing buffer_id (the import-cache key) would be garbage.
// Fields its struct did not reach read as zero, and a missing buffer_id is
// synthesised (see synth_buffer_id). False for a struct too small to carry the
// plane data, whose fds cannot be trusted either.
bool NormalizeFrame(IhsPluginView* v, const IhsFrame* in, IhsFrame* out) {
  *out = IhsFrame{};
  if (in == nullptr || in->struct_size < offsetof(IhsFrame, plane_stride) +
                                             sizeof(out->plane_stride)) {
    return false;
  }
  const size_t copy =
      in->struct_size < sizeof(IhsFrame) ? in->struct_size : sizeof(IhsFrame);
  std::memcpy(out, in, copy);
  out->struct_size = sizeof(IhsFrame);
  // buffer_id absent from the plugin's struct read as 0, which would key every
  // frame on one cache slot and freeze on the first. Give each such frame a
  // fresh id so it is imported rather than aliased.
  if (in->struct_size <
      offsetof(IhsFrame, buffer_id) + sizeof(out->buffer_id)) {
    out->buffer_id =
        v->rolling_buffer_id.fetch_add(1, std::memory_order_relaxed);
    v->synth_buffer_id.store(true, std::memory_order_relaxed);
  }
  return true;
}

#if IVI_HAVE_VULKAN
// Import an extra layer's frame, or reuse the cached import of its buffer_id.
// Consumes the frame's fds either way. Null when the import fails. A fresh
// import keeps scanout dups beside it, as layer 0's does. Caller holds
// v->mutex.
DmabufVulkanImporter::ImportedImage* ImportOrReuseVulkanLocked(
    IhsPluginView* v,
    const IhsFrame& frame) {
  const auto it = v->buffers.find(frame.buffer_id);
  if (it != v->buffers.end() && it->second.width == frame.width &&
      it->second.height == frame.height) {
    CloseFrameFds(&frame);  // a redundant handle to the cached import
    return &it->second;
  }
  if (it != v->buffers.end()) {
    RetireVulkanImportLocked(v, frame.buffer_id);  // re-created at a new size
  }
  IhsPluginView::ScanoutBuffer sb;
  const bool scanout_ok = DupForScanout(frame, &sb);
  DmabufVulkanImporter::ImportedImage imported;
  if (!g_importer.Import(frame, &imported)) {
    CloseFrameFds(&frame);  // import left the fds untouched on failure
    CloseScanoutBuffer(&sb);
    return nullptr;
  }
  if (scanout_ok) {
    CloseScanoutBuffer(&v->scanout[frame.buffer_id]);
    v->scanout[frame.buffer_id] = sb;
  } else if (const auto sit = v->scanout.find(frame.buffer_id);
             sit != v->scanout.end()) {
    CloseScanoutBuffer(&sit->second);
    v->scanout.erase(sit);
  }
  return &v->buffers.emplace(frame.buffer_id, imported).first->second;
}
#endif

// Take a layer list's extra layers (or, for a plain ihs_pv_submit, drop them)
// in the same lock hold as layer 0. Caller holds v->mutex.
void ApplyLayerListLocked(IhsPluginView* v,
                          LayerListUpdate* update,
                          const bool vulkan) {
  (void)vulkan;
  v->layer0_hidden = false;
  if (update == nullptr) {
    v->ClearExtraLayersLocked();
    return;
  }
  std::vector<uint32_t> order;
  order.reserve(update->extras.size());
#if IVI_HAVE_EGL
  // Buffers whose release eventfd this submit already created, starting with
  // layer 0's. A buffer shown by two layers has one release: a second
  // HandBackReleaseEventfd would signal the first as stale.
  std::vector<uint32_t> released_here;
  if (!vulkan && v->pending_egl.valid) {
    released_here.push_back(v->pending_egl.frame.buffer_id);
  }
#endif
  for (ExtraSubmit& es : update->extras) {
    IhsPluginView::ExtraLayer& x = v->extra_layers[es.layer_id];
    order.push_back(es.layer_id);
    HandBackReleaseFence(v, es.acquire_fd, es.out_release_fd);
#if IVI_HAVE_VULKAN
    if (vulkan) {
      if (x.acquire_fd >= 0) {
        close(x.acquire_fd);  // superseded before the compositor took it
      }
      x.acquire_fd = es.acquire_fd;
      if (v->deferred_retire.Resubmitted(es.frame.buffer_id)) {
        RetireVulkanImportLocked(v, es.frame.buffer_id);
      }
      if (auto* img = ImportOrReuseVulkanLocked(v, es.frame); img != nullptr) {
        x.current = img;
        x.current_buffer_id = es.frame.buffer_id;
        x.layout = VK_IMAGE_LAYOUT_GENERAL;
        x.geom = es.geom;
      }
      continue;
    }
#endif
#if IVI_HAVE_EGL
    if (x.pending.valid) {
      // Superseded before it was ever drawn: unless a plane has it, nothing
      // else will release its slot.
      if (v->OwnsSupersededReleaseLocked(x.offer, x.pending)) {
        v->SignalRelease(x.pending.frame.buffer_id);
      }
      CloseFrameFds(&x.pending.frame);
      if (x.pending.acquire_fence_fd >= 0) {
        close(x.pending.acquire_fence_fd);
      }
    }
    x.pending = {};
    x.pending.frame = es.frame;     // takes the plane fds
    x.pending.frame.hdr = nullptr;  // HDR metadata is the view's, from layer 0
    x.pending.acquire_fence_fd = es.acquire_fd;
    x.pending.stash_seq = v->submit_seq;
    x.pending.reimport = v->deferred_retire.Resubmitted(es.frame.buffer_id);
    x.pending.geom = es.geom;
    x.pending.valid = true;
    const uint32_t bid = es.frame.buffer_id;
    if (std::find(released_here.begin(), released_here.end(), bid) ==
        released_here.end()) {
      v->HandBackReleaseEventfd(bid, es.out_release_fd);
      released_here.push_back(bid);
    } else if (const auto efd = v->release_efds.find(bid);
               efd != v->release_efds.end() && es.out_release_fd != nullptr) {
      // The same buffer again: hand back the same release, not a second one.
      if (*es.out_release_fd >= 0) {
        close(*es.out_release_fd);  // HandBackReleaseFence's dup, superseded
      }
      *es.out_release_fd = ::dup(efd->second);
    }
#endif
  }
  // Layers missing from this list are gone.
  for (auto it = v->extra_layers.begin(); it != v->extra_layers.end();) {
    if (std::find(order.begin(), order.end(), it->first) == order.end()) {
#if IVI_HAVE_VULKAN
      if (it->second.acquire_fd >= 0) {
        close(it->second.acquire_fd);
      }
#endif
#if IVI_HAVE_EGL
      if (it->second.pending.valid) {
        if (v->OwnsSupersededReleaseLocked(it->second.offer,
                                           it->second.pending)) {
          v->SignalRelease(it->second.pending.frame.buffer_id);
        }
        CloseFrameFds(&it->second.pending.frame);
        if (it->second.pending.acquire_fence_fd >= 0) {
          close(it->second.pending.acquire_fence_fd);
        }
      }
#endif
      it = v->extra_layers.erase(it);
    } else {
      ++it;
    }
  }
  v->extra_order = std::move(order);
  update->applied = true;
}

// Swap in layer 0's frame (and, through @p update, the rest of the list).
// Consumes @p frame's fds and @p acquire_fence_fd on every path; @p update's
// only when it returns having applied it (update->applied).
int SubmitFrame0(void* user_data,
                 IhsPluginView* v,
                 const IhsFrame* frame,
                 int acquire_fence_fd,
                 int* out_release_fence_fd,
                 const uint32_t layer_id,
                 const ICompositorSurface::LayerGeometry& geom,
                 LayerListUpdate* update) {
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
      // A frame the DRM scene path never took (superseded before it was
      // offered) is never scanned out, so no plane release will fire for it --
      // signal its release here so the producer reclaims that slot. A frame
      // that WAS delivered rides the plane release and must not be
      // double-signalled.
      if (v->OwnsSupersededReleaseLocked(v->offer0, v->pending_egl)) {
        v->SignalRelease(v->pending_egl.frame.buffer_id);
      }
      CloseFrameFds(&v->pending_egl.frame);  // superseded before import
      if (v->pending_egl.acquire_fence_fd >= 0) {
        close(v->pending_egl.acquire_fence_fd);
      }
    }
    v->pending_egl.frame = *frame;  // take ownership of the plane fds
    // Copy the HDR metadata contents while the plugin's pointer is still valid,
    // then null the retained pointer (it aims at plugin-owned memory).
    v->pending_egl.has_hdr = frame->hdr != nullptr;
    if (v->pending_egl.has_hdr) {
      v->pending_egl.hdr = *frame->hdr;
    }
    v->pending_egl.frame.hdr = nullptr;  // do not retain the plugin's hdr ptr
    v->pending_egl.acquire_fence_fd = acquire_fence_fd;  // ownership moves here
    v->pending_egl.stash_seq = v->submit_seq;
    v->pending_egl.reimport = v->deferred_retire.Resubmitted(frame->buffer_id);
    v->pending_egl.geom = geom;
    v->pending_egl.valid = true;
    v->layer0_id = layer_id;
    // Release retired frames this one pushed off screen. Checked here as well
    // as at import: a view on a KMS plane is fed through GetDmabuf and may
    // never reach GetGlTextureName.
    for (const uint32_t id : v->deferred_retire.ReleaseOffScreen(
             [v](uint32_t id) { return v->OnScreenEglLocked(id); })) {
      v->RetireEglImportLocked(id);
    }
    // Per-frame release eventfd: hand the producer a dup to wait on before it
    // reuses this ring slot; the compositor signals our copy from
    // OnScanoutRelease. A stale entry for this buffer_id (the producer reused
    // the slot without our having signalled) is retired first.
    v->HandBackReleaseEventfd(frame->buffer_id, out_release_fence_fd);
    ApplyLayerListLocked(v, update, /*vulkan=*/false);
    // Extra layers the list dropped may have been showing a retired frame.
    for (const uint32_t id : v->deferred_retire.ReleaseOffScreen(
             [v](uint32_t id) { return v->OnScreenEglLocked(id); })) {
      v->RetireEglImportLocked(id);
    }
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

  if (v->deferred_retire.Resubmitted(frame->buffer_id)) {
    // Retired while on screen and now submitted again, possibly for a new
    // dma-buf: the cached import is stale, so import afresh.
    RetireVulkanImportLocked(v, frame->buffer_id);
  }
  const auto it = v->buffers.find(frame->buffer_id);
  if (it != v->buffers.end() && it->second.width == frame->width &&
      it->second.height == frame->height) {
    // Known ring buffer, unchanged size: the submitted fd is a redundant handle
    // to the same memory. Close it and reuse the existing import -- and the
    // scanout dup taken when this id was first imported, which aliases the
    // same memory and is still live.
    CloseFrameFds(frame);
    v->current = &it->second;
    v->current_buffer_id = frame->buffer_id;
    v->current_buffer_valid = true;
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
      // The scanout dup aliases the memory this import is being retired for,
      // so it goes now rather than on the import's reap margin -- the plane,
      // if any, holds its own reference.
      if (const auto sit = v->scanout.find(frame->buffer_id);
          sit != v->scanout.end()) {
        CloseScanoutBuffer(&sit->second);
        v->scanout.erase(sit);
      }
    }
    // Dup for scanout first: Import consumes the fds, and after it there is
    // nothing left to give a plane. A dup that fails is not fatal -- the view
    // is simply composited rather than placed -- so the frame still imports.
    IhsPluginView::ScanoutBuffer sb;
    const bool scanout_ok = DupForScanout(*frame, &sb);

    DmabufVulkanImporter::ImportedImage imported;
    if (!g_importer.Import(*frame, &imported)) {
      CloseFrameFds(frame);  // import left the fds untouched on failure
      CloseScanoutBuffer(&sb);
      return IHS_PV_ERR_INVALID;
    }
    if (scanout_ok) {
      CloseScanoutBuffer(&v->scanout[frame->buffer_id]);
      v->scanout[frame->buffer_id] = sb;
    } else {
      v->scanout.erase(frame->buffer_id);
    }
    // Import consumed plane_fd[0]; a single-plane RGB frame owns no other fds.
    auto [pos, inserted] = v->buffers.emplace(frame->buffer_id, imported);
    v->current = &pos->second;
    v->current_buffer_id = frame->buffer_id;
    v->current_buffer_valid = scanout_ok;
    // Synthesised ids never repeat, so every earlier entry is dead. Retire them
    // (the reap margin covers a compositor present still binding one) so the
    // cache holds just the current import rather than growing per frame.
    if (v->synth_buffer_id.load(std::memory_order_relaxed)) {
      for (auto bit = v->buffers.begin(); bit != v->buffers.end();) {
        if (bit->first != frame->buffer_id) {
          v->retired.push_back(
              {bit->second, v->submit_seq + kImportRetireMargin});
          if (const auto sit = v->scanout.find(bit->first);
              sit != v->scanout.end()) {
            CloseScanoutBuffer(&sit->second);
            v->scanout.erase(sit);
          }
          bit = v->buffers.erase(bit);
        } else {
          ++bit;
        }
      }
    }
  }

  // A retired frame that was on screen is off it now.
  for (const uint32_t id : v->deferred_retire.ReleaseOffScreen(
           [v](uint32_t id) { return OnScreenVulkanLocked(v, id); })) {
    RetireVulkanImportLocked(v, id);
  }

  // The plugin re-rendered the buffer before submitting, so the compositor
  // transitions from GENERAL to read it. A spec-correct foreign-queue-family
  // acquire from the producer is the explicit-sync increment.
  v->current_layout = VK_IMAGE_LAYOUT_GENERAL;
  v->geom0 = geom;
  v->layer0_id = layer_id;
  ApplyLayerListLocked(v, update, /*vulkan=*/true);
  // A retired frame an extra layer was showing may be off screen now as well.
  for (const uint32_t id : v->deferred_retire.ReleaseOffScreen(
           [v](uint32_t id) { return OnScreenVulkanLocked(v, id); })) {
    RetireVulkanImportLocked(v, id);
  }
  lock.unlock();  // don't call into the engine holding the view lock
  ScheduleEngineFrame(user_data);
  return IHS_PV_OK;
}
#endif  // IVI_HAVE_VULKAN

// ihs_pv_submit: one full-view layer, which also clears any layers a previous
// ihs_pv_submit_layers left above it.
int HostSubmit(void* user_data,
               IhsPlatformView* view,
               const IhsFrame* frame,
               int acquire_fence_fd,
               int* out_release_fence_fd) {
  // Release fence (compositor -> producer, #338): -1 by default, replaced under
  // v->mutex in each backend path with a dup of this view's latest fence (see
  // HandBackReleaseFence). -1 stands until the first composite has run, or if
  // the dup fails; the producer owns and closes any fd handed back.
  if (out_release_fence_fd != nullptr) {
    *out_release_fence_fd = -1;
  }
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  IhsFrame normalized{};
  if (!NormalizeFrame(v, frame, &normalized)) {
    // Rejected, and not closed: plane_count/plane_fd cannot be trusted.
    if (acquire_fence_fd >= 0) {
      close(acquire_fence_fd);
    }
    ihs::log::warn(
        "[ihs_pv] submit rejected: IhsFrame struct_size {} too small",
        frame != nullptr ? frame->struct_size : 0);
    return IHS_PV_ERR_INVALID;
  }
  return SubmitFrame0(user_data, v, &normalized, acquire_fence_fd,
                      out_release_fence_fd, /*layer_id=*/0,
                      ICompositorSurface::LayerGeometry{}, nullptr);
}

// The geometry a layer asks for, in the form the compositor draws with.
ICompositorSurface::LayerGeometry GeometryOf(const IhsLayer& l) {
  ICompositorSurface::LayerGeometry g;
  constexpr double kFixed = 65536.0;  // 16.16
  g.src = {l.src_x / kFixed, l.src_y / kFixed, l.src_w / kFixed,
           l.src_h / kFixed};
  const auto clamp = [](uint32_t v) {
    return static_cast<int32_t>(v > INT32_MAX ? INT32_MAX : v);
  };
  g.dst = {l.dst_x, l.dst_y, clamp(l.dst_w), clamp(l.dst_h)};
  g.transform =
      l.transform <= static_cast<uint32_t>(BufferTransform::kFlipped270)
          ? static_cast<BufferTransform>(l.transform)
          : BufferTransform::kNormal;
  g.opaque = l.opaque != 0;
  return g;
}

// ihs_pv_submit_layers. libihs_shared has validated the list's shape; this
// owns every fd in it from here, on every path.
int HostSubmitLayers(void* user_data,
                     IhsPlatformView* view,
                     const IhsLayer* layers,
                     const size_t layer_count,
                     uint64_t /* seq: reserved for presentation feedback */,
                     int* out_release_fence_fds) {
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  const auto close_from = [&](size_t first) {
    for (size_t i = first; i < layer_count; ++i) {
      CloseFrameFds(layers[i].frame);
      if (layers[i].acquire_fence_fd >= 0) {
        close(layers[i].acquire_fence_fd);
      }
    }
  };
  if (layer_count == 0) {
    {
      const std::lock_guard<std::mutex> lock(v->mutex);
      v->ClearExtraLayersLocked();
      v->layer0_hidden = true;
    }
    ScheduleEngineFrame(user_data);
    return IHS_PV_OK;
  }
  // A layer_id twice would make two layers share one layer's state.
  for (size_t i = 0; i < layer_count; ++i) {
    for (size_t j = i + 1; j < layer_count; ++j) {
      if (layers[i].layer_id == layers[j].layer_id) {
        ihs::log::warn("[ihs_pv] submit_layers rejected: layer_id {} repeats",
                       layers[i].layer_id);
        close_from(0);
        return IHS_PV_ERR_INVALID;
      }
    }
  }
  // Every frame here carries a buffer_id (libihs_shared checked), so none is
  // synthesised, and the synthesised-id pruning -- which would retire other
  // layers' imports -- stops applying to this view.
  v->synth_buffer_id.store(false, std::memory_order_relaxed);
  LayerListUpdate update;
  update.extras.resize(layer_count - 1);
  IhsFrame frame0{};
  for (size_t i = 0; i < layer_count; ++i) {
    IhsFrame* dst = i == 0 ? &frame0 : &update.extras[i - 1].frame;
    if (!NormalizeFrame(v, layers[i].frame, dst)) {
      close_from(0);  // cannot happen past libihs_shared's check; be safe
      return IHS_PV_ERR_INVALID;
    }
  }
  for (size_t i = 1; i < layer_count; ++i) {
    ExtraSubmit& es = update.extras[i - 1];
    es.layer_id = layers[i].layer_id;
    es.acquire_fd = layers[i].acquire_fence_fd;
    es.out_release_fd =
        out_release_fence_fds != nullptr ? &out_release_fence_fds[i] : nullptr;
    es.geom = GeometryOf(layers[i]);
  }
  const int rc = SubmitFrame0(
      user_data, v, &frame0, layers[0].acquire_fence_fd,
      out_release_fence_fds != nullptr ? &out_release_fence_fds[0] : nullptr,
      layers[0].layer_id, GeometryOf(layers[0]), &update);
  if (!update.applied) {
    close_from(1);  // layer 0 failed; the rest were never taken
  }
  return rc;
}

// Process-global host; user_data re-points at the most recently installed
// engine (single-engine today).
const char* HostAssetsPath(void* user_data) {
  const auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->flutter_asset_directory.empty()) {
    return nullptr;
  }
  // Borrowed: owned by the engine state, which outlives every platform view.
  return state->flutter_asset_directory.c_str();
}

// Queue a plugin task on the platform runner's strand, the same one the
// registry's callbacks and ScheduleEngineFrame run on, so it is ordered with
// them. The task is the plugin's code; it does not touch the engine, so it
// needs no shutdown re-check on arrival.
int HostPostPlatformTask(void* user_data,
                         IhsPvTaskFn fn,
                         void* task_user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr) {
    return IHS_PV_ERR_NO_BACKEND;
  }
  TaskRunner* runner = state->platform_task_runner;
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return IHS_PV_ERR_NO_BACKEND;  // before start-up or after teardown
  }
  asio::post(*runner->GetStrandContext(),
             [fn, task_user_data]() { fn(task_user_data); });
  return IHS_PV_OK;
}

int HostIsPlatformThread(void* user_data) {
  const auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->platform_task_runner == nullptr) {
    return 0;
  }
  return state->platform_task_runner->IsThreadEqual(pthread_self()) ? 1 : 0;
}

// The producer is done with the dma-buf `buffer_id` names. Drop its imports
// now, or -- when it is on screen -- once a later frame supersedes it. Any
// thread, under the same dispose rule as submit.
int HostRetireBuffer(void* /* user_data */,
                     IhsPlatformView* view,
                     const uint32_t buffer_id) {
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  const std::lock_guard<std::mutex> lock(v->mutex);
  // The plane path caches a framebuffer per scanout key. Tell it this one is
  // finished with, and move the id to a new generation, so that if it is
  // submitted again -- for this memory or another -- its frames reach a plane
  // under a new key and are imported afresh.
  if (v->retired_scanout_keys.size() >= IhsPluginView::kMaxRetiredScanoutKeys) {
    v->retired_scanout_keys.erase(v->retired_scanout_keys.begin());
  }
  v->retired_scanout_keys.push_back(ICompositorSurface::ScanoutKey(
      buffer_id, v->GenerationLocked(buffer_id)));
  v->BumpGenerationLocked(buffer_id);
  bool on_screen = false;
#if IVI_HAVE_VULKAN
  on_screen = on_screen || OnScreenVulkanLocked(v, buffer_id);
#endif
#if IVI_HAVE_EGL
  on_screen = on_screen || v->OnScreenEglLocked(buffer_id);
#endif
  if (!v->deferred_retire.Retire(buffer_id, on_screen)) {
    return IHS_PV_OK;  // dropped when superseded
  }
#if IVI_HAVE_VULKAN
  RetireVulkanImportLocked(v, buffer_id);
#endif
#if IVI_HAVE_EGL
  v->RetireEglImportLocked(buffer_id);
#endif
  return IHS_PV_OK;
}

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
  g_host.assets_path = HostAssetsPath;
  g_host.vulkan_context = HostVulkanContext;
  g_host.egl_context = HostEglContext;
  g_host.grant = HostGrant;
  g_host.revoke = HostRevoke;
  g_host.grant_drm_plane_id = HostGrantDrmPlaneId;
  g_host.grant_shm_fd = HostGrantShmFd;
  g_host.submit = HostSubmit;
  g_host.post_platform_task = HostPostPlatformTask;
  g_host.is_platform_thread = HostIsPlatformThread;
  g_host.retire_buffer = HostRetireBuffer;
  g_host.submit_layers = HostSubmitLayers;
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
