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

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/modeset/modeset.hpp>
#include <drm-cxx/planes/allocator.hpp>
#include <drm-cxx/planes/output.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <shell/platform/embedder/embedder.h>

#include "config/common.h"

#if USE_DRM_SCENE
#include <drm-cxx/scene/layer_scene.hpp>
#endif

#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"

class DrmBackend;
class ICompositorSurface;
class GbmBackingStoreLayerSource;

// Default pool sizes used by CreateGbmStore. Flutter backing-stores
// rotate among N=3 BOs so direct-scanout never binds the BO that's
// currently being written.
// Composition buffers (`comp_bufs_`) and the background store
// (`bg_store_`) stay single-buffered: the former are already
// multi-buffered at the `comp_idx_` level (kNumCompBufs=2), the latter
// is a single-shot scanout target with no per-frame writes.
inline constexpr size_t kFlutterBsPoolSize = 3;
inline constexpr size_t kCompBufPoolSize = 1;
inline constexpr size_t kBgStorePoolSize = 1;

// One renderable surface that Flutter composes into via FBO. The pool
// rotates BOs so direct-scanout never binds the BO that's currently
// being written. Each Slot owns the per-frame storage (BO, EGLImage,
// color texture, KMS FB id); the BS-level FBO and depth/stencil
// renderbuffer are shared and rebound across slots.
struct GbmBackingStore {
  // Compile-time upper bound on slots-per-store. Per-instance
  // `pool_size` selects how many are actually allocated; unused slots
  // stay zero-initialized.
  static constexpr size_t kMaxPoolSize = 3;

  // Per-slot state. State drives the rotate in PresentLayers:
  //   Free    — available for next render
  //   Active  — FBO color attachment points here; Flutter is rendering
  //   Pending — atomic-committed; awaiting PAGE_FLIP_EVENT
  struct Slot {
    gbm_bo* bo = nullptr;
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    GLuint color_tex = 0;
    // Lazily populated by EnsureDrmFbId() on first direct-scanout use
    // and cached for the BO's lifetime — stays 0 on paths that only
    // sample the store as a GL texture (e.g. framed mode, GL fallback),
    // avoiding a per-BO drmModeAddFB2/RmFB syscall pair that never
    // earned its keep.
    uint32_t drm_fb_id = 0;
    enum class State : uint8_t { Free, Active, Pending } state = State::Free;
  };

  std::array<Slot, kMaxPoolSize> pool{};
  size_t pool_size = 1;
  size_t active_idx = 0;

  // Shared across slots: one FBO whose GL_COLOR_ATTACHMENT0 we rebind
  // as the pool rotates, plus a single scratch depth/stencil RB (cleared
  // each frame; need not be multi-buffered).
  GLuint fbo = 0;
  GLuint depth_stencil_rb = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;

  [[nodiscard]] Slot& active() noexcept { return pool[active_idx]; }
  [[nodiscard]] const Slot& active() const noexcept { return pool[active_idx]; }
};

// FlutterCompositor for the DRM/KMS backend with hardware-plane overlay
// support.
//
// Each Flutter-rendered layer is a GBM BO that can be scanned out directly
// on a KMS plane (via drm_fb_id) or composited into a composition buffer
// via GL when hardware planes are exhausted.
//
// PresentLayers builds a drm::planes::Output, runs the Allocator to assign
// layers to planes, GL-composites any overflow into a double-buffered
// composition GBM BO, and atomic-commits the result.
class DrmCompositor {
 public:
  explicit DrmCompositor(DrmBackend* backend);
  ~DrmCompositor();

  DrmCompositor(const DrmCompositor&) = delete;
  DrmCompositor& operator=(const DrmCompositor&) = delete;

  bool CreateBackingStore(const FlutterBackingStoreConfig* config,
                          FlutterBackingStore* out);
  bool CollectBackingStore(const FlutterBackingStore* store);
  bool PresentLayers(const FlutterLayer** layers, size_t layer_count);

  void RegisterSurface(FlutterPlatformViewIdentifier id,
                       std::shared_ptr<ICompositorSurface> surface);
  void UnregisterSurface(FlutterPlatformViewIdentifier id);
  void ResizeSurface(FlutterPlatformViewIdentifier id,
                     int32_t width,
                     int32_t height);

  // Session pause/resume hooks, invoked by DrmBackend from the
  // DrmSession dispatch thread. SetPaused(true) gates the Present*
  // entry points so they ack the frame without touching the kernel;
  // OnResume() clears the pause flag, drops the stale flip-pending
  // latch (no PAGE_FLIP_EVENT will come for the suppressed commits),
  // and forces the next commit to be a full modeset.
  void SetPaused(bool paused);
  void OnResume();

  // Tell the compositor which DRM plane an externally-managed cursor
  // (drm::cursor::Renderer) is armed on, so the scene allocator never
  // disables it. On a CRTC with no dedicated cursor plane the cursor
  // takes an overlay this scene would otherwise treat as unused;
  // without the reservation the disable-unused pass turns it off every
  // commit, fighting the cursor's own commits (flicker on motion).
  // Stored and (re)applied to the LayerScene whenever it is built.
  // plane_id == 0 (legacy cursor path / no cursor) is a no-op.
  void ReserveCursorPlane(uint32_t plane_id);

  // Per-flip-complete work for the atomic plane path. Called by
  // DrmBackend::UnifiedPageFlipHandler on the platform task runner
  // thread when planes_active() returns true. Just clears
  // flip_pending_ and records the frame completion; baton return
  // happens in the unified handler.
  void OnFlipComplete();

  // Queried by DrmBackend::SetVsyncBaton to decide whether a baton
  // arriving from Flutter can wait for the asio flip monitor to
  // deliver it (yes, if a flip is in flight) or must be kicked
  // inline (no, if the pipeline is idle — otherwise the baton would
  // never reach Flutter).
  [[nodiscard]] bool IsFlipPending() const {
    return flip_pending_.load(std::memory_order_acquire);
  }

 private:
  struct StoreBaton {
    DrmCompositor* owner;
    GbmBackingStore* store;
#if USE_DRM_SCENE
    // Borrowed LayerBufferSource adapter — owned by the LayerScene
    // once add_layer() consumes it; nullptr after that. CreateBackingStore
    // constructs it; PresentLayers (under USE_DRM_SCENE) moves it into
    // the scene the first time the store appears in a FlutterLayer.
    // CollectBackingStore destroys the wrapper if it never reached the
    // scene (e.g. the store retired before its first Present).
    std::unique_ptr<GbmBackingStoreLayerSource> scene_source;
#endif
  };

  bool InitEglExtensions();
  bool InitPlaneAllocator();
  bool InitCompositionBuffers();
  // Framed-mode setup: pick an overlay plane for the letterboxed
  // composition buffer, cache its property IDs plus the primary plane's,
  // and create a mode-sized opaque "background" GBM store that the
  // primary plane scans out while the overlay carries the framed content.
  // See PresentFramed for the rationale (amdgpu DC rejects partial
  // primary-plane coverage, so primary must always cover the full CRTC).
  bool InitFramedMode();
  void EnsureGlCapsProbed();
  void DestroyGbmStore(GbmBackingStore& store) const;
  // @p force_scanout requests GBM_BO_USE_SCANOUT even before
  // planes_available_ is set — needed for stores created during
  // InitPlaneAllocator (the REFLECT_Y probe / direct-overlay BG), which
  // must be KMS-importable but run before the caller flips the flag.
  // Without it gbm may pick a render-only tiled modifier that AddFB2
  // rejects.
  bool CreateGbmStore(GbmBackingStore& store,
                      uint32_t w,
                      uint32_t h,
                      uint32_t format,
                      size_t pool_size = 1,
                      bool force_scanout = false) const;
  uint32_t ImportBoAsFb(gbm_bo* bo) const;
  // Lazily import the store's BO as a DRM framebuffer on first direct-
  // scanout use. Returns true when @c store.drm_fb_id is non-zero on
  // return (already cached or freshly imported). Backing stores that
  // only participate in GL composition never invoke this — no AddFB2
  // per frame on framed-mode / GL-fallback paths.
  bool EnsureDrmFbId(GbmBackingStore& store) const;

  // Composite @p src_tex into @p target_fbo at the given destination rect.
  // Pass @p src_fbo (the FBO that owns @p src_tex as its color attachment)
  // to enable the blit fast-path; pass 0 when the source is a raw texture
  // with no backing FBO — the quad path will be used instead.
  //
  // @p flip_y must be true whenever the destination FBO is an EGLImage-
  // backed GBM BO scanned out directly by KMS (e.g. @c comp_bufs_): KMS
  // reads memory top-down while GL's window convention has y=0 at the
  // bottom, so Flutter-top-down content lands upside-down without a flip.
  // Leave false for gbm_surface FBO 0 where Mesa already matches NDC y=-1
  // to screen-bottom.
  void CompositeLayerIntoFbo(GLuint target_fbo,
                             GLuint src_fbo,
                             GLuint src_tex,
                             GLsizei src_w,
                             GLsizei src_h,
                             GLint dst_x,
                             GLint dst_y,
                             GLsizei dst_w,
                             GLsizei dst_h,
                             bool blend,
                             bool flip_y = false) const;

  bool WaitForPendingFlip() const;

  // Post-first-commit sanity probe. Confirms the kernel actually honored
  // the modeset by reading CRTC.ACTIVE + primary plane.FB_ID via
  // drmModeObjectGetProperties, then samples vblank sequence across ~2
  // refresh periods to confirm scanout is ticking. Logs a loud error for
  // any mismatch — the "commit returned success but the pipe isn't
  // running" case that otherwise surfaces only as a stalled PresentLayers
  // down the line.
  void VerifyPipeRunning() const;

  // GL fallback: composites all layers into FBO 0 and calls
  // DrmBackend::Present(). Used when the plane allocator isn't
  // available or when all layers need composition anyway.
  bool PresentViaGlFallback(const FlutterLayer** layers, size_t count);

  // Framed-mode present path. Composites every Flutter layer into the
  // mode-independent composition buffer (same pixel work as the GL
  // fallback) and atomic-commits a two-plane layout: primary plane
  // covers the full CRTC with a persistent opaque BG FB, overlay plane
  // scans out the composition buffer centered on the CRTC. Bypasses the
  // drm-cxx Allocator entirely — its "composition layer → primary"
  // convention can't produce a partial-coverage primary and amdgpu DC
  // rejects that anyway.
  bool PresentFramed(const FlutterLayer** layers, size_t count);

#if USE_DRM_SCENE
  // Non-framed present path driven by drm::scene::LayerScene. Walks
  // FlutterLayer[], syncs the scene's layer membership against it via
  // find_by_identity_tag / add_layer / remove_layer keyed on the
  // backing-store baton pointer, updates DisplayParams via
  // set_*_if_changed setters, and runs scene_->commit() in place of
  // the manual AtomicRequest + Allocator path. CommitReport.placements
  // is inspected: any layer reported Composited routes the frame
  // through PresentViaGlFallback instead (the GL composite path is
  // materially faster than drm-cxx's CompositeCanvas over uncached
  // GBM read-back). Platform-view layers force the same fallback —
  // platform-view→scene plumbing is not wired in this revision.
  bool PresentLayersViaScene(const FlutterLayer** layers, size_t count);

  // Direct-overlay setup: pick the REFLECT_Y-capable overlay for the
  // backing-store scanout, cache its property IDs plus the primary
  // plane's (the BG target), and create the mode-sized opaque BG store.
  // Mirrors InitFramedMode's plane selection but requires the overlay
  // to advertise both the BS format and REFLECT_Y. Returns false (→
  // fall back to the scene_ path) if no suitable overlay exists or the
  // BG store can't be created. See direct_overlay_mode_ / the class
  // comment for the rationale.
  bool InitDirectOverlay();

  // Authoritative REFLECT_Y-on-primary probe. The rotation-property enum
  // bitmask (PlaneSupportsReflectY) is unreliable: vc4 (and amdgpu DC)
  // primaries advertise REFLECT_Y yet the kernel rejects it at commit
  // with EINVAL. This runs a one-shot TEST_ONLY atomic commit binding
  // the opaque BG to the primary with ROTATE_0|REFLECT_Y; if that TEST
  // fails but the same commit with plain ROTATE_0 passes, REFLECT_Y is
  // the culprit and the primary genuinely can't do it. Sets up bg_store_
  // as the probe FB (reused by InitDirectOverlay). Returns the verdict;
  // on an inconclusive probe (both variants fail) returns @p enum_hint.
  bool ProbePrimaryReflectY(uint32_t primary_id, bool enum_hint);

  // Direct-overlay present path. Zero-copy analogue of PresentFramed:
  // pins the opaque BG to the primary (full CRTC) and atomic-commits
  // the single Flutter backing store directly on the overlay with
  // REFLECT_Y, no GL composite. Falls back to PresentViaGlFallback for
  // any frame that isn't exactly one backing-store layer (multi-layer /
  // platform-view frames are a follow-on).
  bool PresentDirectOverlay(const FlutterLayer** layers, size_t count);
#endif

  DrmBackend* backend_;

  // EGL image extensions.
  PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;

  // EGL native-fence sync. Used on the direct-scanout path to hand the
  // kernel an `IN_FENCE_FD` so it waits for Flutter's GPU writes to
  // retire before scanning the BO.
  // Required on drivers that don't insert implicit dma-fence on commit
  // (notably nvidia-drm). If the extension is missing, the path falls
  // back to a synchronous glFinish before commit (correctness preserved
  // at the cost of a per-frame stall).
  PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR_ = nullptr;
  PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR_ = nullptr;
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID_ = nullptr;
  bool has_native_fence_sync_ = false;

  GlCaps gl_caps_{};
  bool gl_caps_probed_{false};
  std::unique_ptr<GlCompositor> gl_compositor_;

  std::unordered_map<StoreBaton*, std::unique_ptr<GbmBackingStore>> stores_;

  // Idle backing stores awaiting re-use. CollectBackingStore moves a
  // retired store here instead of destroying it; the next matching
  // CreateBackingStore pops it. Keeps GBM BO + EGLImage + GL FBO/tex/RB
  // (and, if already imported, the KMS FB) alive across Flutter's
  // Create/Collect churn. Linear-scan match on (w, h, format) is fine —
  // in practice the pool holds 1–2 entries of a single size.
  std::vector<std::unique_ptr<GbmBackingStore>> store_pool_;

  size_t store_create_total_{0};
  size_t store_collect_total_{0};
  size_t store_peak_live_{0};
  size_t store_pool_hits_{0};
  size_t store_pool_misses_{0};

  mutable std::mutex surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      surfaces_;

  GLuint texture_blit_fbo_{0};

  // Plane allocation.
  std::optional<drm::planes::PlaneRegistry> plane_registry_;
  std::unique_ptr<drm::planes::Allocator> allocator_;
  drm::planes::Layer comp_layer_;  // composition layer descriptor
  bool planes_available_{false};

  // Primary plane's zpos (queried from the registry at init). Flutter
  // layer 0 is placed at this zpos so the allocator assigns it to the
  // primary plane; later layers stack above at primary_zpos_ + N.
  // Immutable on most drivers (amdgpu=2, i915=0, meson=0, …) — we pick
  // whatever the hardware advertises.
  uint64_t primary_zpos_{0};

  // Owns the MODE_ID / ACTIVE / Connector.CRTC_ID properties + mode
  // blob for atomic modesets. attach()-ed to the first commit.
  std::optional<drm::modeset::Modeset> modeset_;

#if USE_DRM_SCENE
  // LayerScene-driven non-framed present path. Constructed in
  // InitPlaneAllocator when !framed_; null in framed mode (the
  // primary-BG + overlay layout keeps the manual atomic path).
  // PresentLayers's non-framed branch routes per-frame add_layer /
  // find_by_identity_tag / commit through this scene; CreateBackingStore
  // produces matching LayerBufferSource wrappers stashed in StoreBaton
  // until their first Present add_layer()s them in.
  std::unique_ptr<drm::scene::LayerScene> scene_;

  // DRM plane id of an externally-managed cursor to reserve from the
  // scene allocator's disable-unused pass. Set by ReserveCursorPlane()
  // (from DrmBackend, once the cursor exists); applied to scene_ each
  // time the scene is built. 0 = none / legacy cursor path.
  uint32_t cursor_reserved_plane_{0};

  // Push cursor_reserved_plane_ into the live scene (if any). No-op
  // when there's no scene or no cursor plane. Idempotent; called both
  // when the reservation is set and whenever the scene is (re)built.
  void ApplyCursorReservation();

  // Embedder-side mirror of the scene's live layer set, keyed by
  // baton pointer (the same pointer set as LayerDesc::identity_tag).
  // Walked each frame to prune layers whose baton didn't appear in
  // the current FlutterLayer[] — drm-cxx's scene retains layers
  // across commits and exposes no iteration API beyond
  // find_by_identity_tag, so the prune list lives here. Single-digit
  // capacity in steady state; std::vector + linear scan is faster
  // than any hashing for that size.
  std::vector<StoreBaton*> scene_layer_batons_;
#endif

  // Double-buffered composition buffer for layers that overflow HW planes.
  static constexpr int kNumCompBufs = 2;
  GbmBackingStore comp_bufs_[kNumCompBufs];
  int comp_idx_{0};
  bool comp_bufs_valid_{false};

  // ── Framed mode (fb_w < mode_w or fb_h < mode_h) ──────────────────
  // amdgpu DC (and some other atomic drivers) reject a primary plane
  // whose CRTC rect doesn't cover the full CRTC. For framed configs we
  // pin the primary plane to a mode-sized opaque BG FB and drive the
  // framed composition buffer on an overlay plane at the letterbox
  // offset. bg_store_ is filled with black once at init and never
  // changes; only the overlay's FB_ID flips per frame.
  bool framed_{false};
  GbmBackingStore bg_store_{};
  bool bg_store_valid_{false};
  uint32_t framed_primary_id_{0};
  uint32_t framed_overlay_id_{0};
  uint64_t framed_overlay_zpos_{0};
  drm::PropertyStore framed_props_;

  // ── Direct-overlay mode (zero-copy scene present) ─────────────────
  // For non-framed configs on hardware whose PRIMARY plane lacks
  // REFLECT_Y (amdgpu DC, RPi5 vc4) but whose overlays support it: pin
  // an opaque BG to the primary (full CRTC coverage, as amdgpu DC
  // requires) and direct-scan the single Flutter backing store on a
  // REFLECT_Y-capable overlay so the GL-rendered (bottom-up) bits flip
  // to scanout order without a copy. Reuses framed_primary_id_ (the BG
  // target), framed_props_ (cached plane props), and bg_store_ (the
  // opaque BG) — same plane shape as framed mode, but the overlay
  // direct-scans the BS instead of carrying a composited buffer.
  // direct_overlay_mode_ gates PresentDirectOverlay in PresentLayers;
  // it stays false (and the scene_ path runs) when the primary already
  // supports REFLECT_Y or no overlay qualifies.
  bool direct_overlay_mode_{false};
  uint32_t direct_overlay_id_{0};
  uint64_t direct_overlay_zpos_{0};

  // Atomic-commit flip state. Written by
  // DrmBackend::UnifiedPageFlipHandler → OnFlipComplete on the
  // platform task runner thread (when the kernel reports flip
  // completion); also written false on session pause / resume. Read
  // by the rasterizer thread inside WaitForPendingFlip and on the
  // commit paths. Atomic because the writes and reads cross threads.
  std::atomic<bool> flip_pending_{false};

  // Backing-store slot release pipeline. PresentLayers pushes the
  // just-committed Pending slots onto in_flight_slots_ on the
  // rasterizer thread; OnFlipComplete pops them on the platform task
  // runner thread when PAGE_FLIP_EVENT arrives. The slots currently
  // being scanned out are tracked in scanning_slots_ so they can be
  // freed at the NEXT flip event (the event that displaces them as
  // the active scanout).
  struct SlotRef {
    GbmBackingStore* store;
    size_t slot_idx;
  };
  std::mutex slot_pipeline_mu_;
  std::deque<std::vector<SlotRef>> in_flight_slots_;
  std::vector<SlotRef> scanning_slots_;

  // First atomic commit after Create(). The kernel needs the modeset
  // flag + a blocking commit; subsequent commits use NONBLOCK +
  // PAGE_FLIP_EVENT.
  bool plane_mode_set_{false};

  // Set once we hit an unrecoverable atomic-commit failure. All future
  // frames route through PresentViaGlFallback until restart.
  bool fallback_latched_{false};

  // Session-pause gate. Set by DrmBackend::OnSessionPaused (libseat
  // disable_seat) and cleared by OnResume on libseat enable_seat. Read
  // by the Present* entry points on the rasterizer thread; written on
  // DrmSession's dispatch thread — must be atomic.
  std::atomic<bool> paused_{false};

  // Probed once in InitPlaneAllocator: true iff at least one plane on
  // the CRTC has reflect-y in its rotation bitmask. Gates the
  // direct-scanout fast path in PresentLayers — without REFLECT_Y
  // available somewhere, BS pixels (rendered bottom-up via GL) would
  // scan out upside-down, so the layer must go through GL composition
  // instead. Per-plane gating (not primary-only) so that hardware
  // where overlays support rotation but primary doesn't can still
  // benefit on the layers where the allocator can use an overlay.
  bool any_plane_supports_reflect_y_{false};

  // Probed once in InitPlaneAllocator: true iff the PRIMARY plane's
  // rotation bitmask includes REFLECT_Y. When false but
  // any_plane_supports_reflect_y_ is true, the non-framed path can't
  // direct-scan the bottom-up BS on the primary (REFLECT_Y write →
  // EINVAL on amdgpu DC / vc4), so it routes through direct-overlay
  // mode (BG on primary, BS on a REFLECT_Y overlay) instead.
  bool primary_supports_reflect_y_{false};

  // One-shot diagnostic: log on the first PresentLayers entry after a
  // resume so a stuck Flutter frame pacer is visible. Cleared by
  // OnResume, set on entry.
  bool resume_pending_logged_{false};

  // Per-frame composite-path profiling state. Opaque struct in
  // drm_compositor.cc so the header doesn't need <chrono>/<atomic>.
  // Only mutated on the rasterizer thread (PresentFramed /
  // PresentLayers), so no synchronization.
  struct FrameProfileState;
  std::unique_ptr<FrameProfileState> profile_;

 public:
  // Queried by DrmBackend to decide whether .present should be a no-op
  // (plane path active) or run the legacy eglSwapBuffers + drmModePageFlip
  // path (fallback latched or plane path disabled).
  [[nodiscard]] bool fallback_latched() const { return fallback_latched_; }
  [[nodiscard]] bool planes_active() const {
    return planes_available_ && !fallback_latched_;
  }
};
