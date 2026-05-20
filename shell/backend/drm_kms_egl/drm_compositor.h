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

#include <atomic>
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

#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"

class DrmBackend;
class ICompositorSurface;

struct GbmBackingStore {
  gbm_bo* bo = nullptr;
  EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
  GLuint fbo = 0;
  GLuint color_tex = 0;
  GLuint depth_stencil_rb = 0;
  // Lazily populated by EnsureDrmFbId() on first direct-scanout use and
  // cached for the BO's lifetime — stays 0 on paths that only sample the
  // store as a GL texture (e.g. framed mode, GL fallback), avoiding a
  // per-BO drmModeAddFB2/RmFB syscall pair that never earned its keep.
  uint32_t drm_fb_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
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

 private:
  struct StoreBaton {
    DrmCompositor* owner;
    GbmBackingStore* store;
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
  bool CreateGbmStore(GbmBackingStore& store,
                      uint32_t w,
                      uint32_t h,
                      uint32_t format) const;
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
  static void PageFlipHandler(int fd,
                              unsigned int sequence,
                              unsigned int tv_sec,
                              unsigned int tv_usec,
                              void* user_data);

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
  // scans out the composition buffer centred on the CRTC. Bypasses the
  // drm-cxx Allocator entirely — its "composition layer → primary"
  // convention can't produce a partial-coverage primary and amdgpu DC
  // rejects that anyway.
  bool PresentFramed(const FlutterLayer** layers, size_t count);

  DrmBackend* backend_;

  // EGL image extensions.
  PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;

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

  // Atomic-commit flip state (compositor owns its own flip lifecycle
  // when using the plane-allocator path).
  bool flip_pending_{false};

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

  // One-shot diagnostic: log on the first PresentLayers entry after a
  // resume so a stuck Flutter frame pacer is visible. Cleared by
  // OnResume, set on entry.
  bool resume_pending_logged_{false};

 public:
  // Queried by DrmBackend to decide whether .present should be a no-op
  // (plane path active) or run the legacy eglSwapBuffers + drmModePageFlip
  // path (fallback latched or plane path disabled).
  [[nodiscard]] bool fallback_latched() const { return fallback_latched_; }
  [[nodiscard]] bool planes_active() const {
    return planes_available_ && !fallback_latched_;
  }
};
