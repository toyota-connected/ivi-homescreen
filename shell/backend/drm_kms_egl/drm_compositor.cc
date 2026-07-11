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

#include "backend/drm_kms_egl/drm_compositor.h"
#include "logging/logging.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm-cxx/core/format.hpp>

#include "backend/drm_kms_egl/driver_probe.h"
#include "backend/drm_kms_egl/drm_backend.h"
#include "backend/drm_kms_egl/drm_cursor.h"
#include "display/drm_display.h"
#if USE_DRM_SCENE
#include "backend/drm_kms_egl/scene_layer_source_gl.h"
#endif
#include "drm-cxx/src/modeset/atomic.hpp"
#include "libflutter_engine.h"
#include "logging.h"
#include "profiling/frame_profile.h"
#include "view/compositor_surface_interface.h"

namespace {

bool AllocDebug() {
  static const bool enabled = std::getenv("DRM_ALLOC_DEBUG") != nullptr;
  return enabled;
}

// Per-frame composite profiling (IVI_DRM_PROFILE, or the IVI_PROFILE umbrella).
// When enabled, the PresentFramed hot path samples a few fence-post timestamps
// and accumulates per-stage means/maxes; every kProfileWindow frames the
// rolling stats get logged and the accumulators reset. clock_gettime is
// single-digit-ns on modern x86 so the overhead is negligible relative to the
// work being measured. Gated via FrameProfile::Enabled so IVI_PROFILE turns it
// on alongside every other backend's profile (IVI_WL/VK/SW/VSYNC_PROFILE).
bool ProfileEnabled() {
  static const bool enabled =
      profiling::FrameProfile::Enabled("IVI_DRM_PROFILE");
  return enabled;
}

uint64_t NsNow() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

constexpr uint32_t kProfileWindow = 60;

struct FrameProfile {
  // Accumulated nanoseconds across the last kProfileWindow frames.
  uint64_t wait_sum_ns{0};
  uint64_t compose_sum_ns{0};
  uint64_t test_sum_ns{0};
  uint64_t commit_sum_ns{0};
  uint64_t total_sum_ns{0};
  // Worst single frame seen in this window.
  uint64_t wait_max_ns{0};
  uint64_t compose_max_ns{0};
  uint64_t test_max_ns{0};
  uint64_t commit_max_ns{0};
  uint64_t total_max_ns{0};
  uint32_t frames{0};

  void account(uint64_t wait,
               uint64_t compose,
               uint64_t test,
               uint64_t commit,
               uint64_t total) {
    wait_sum_ns += wait;
    compose_sum_ns += compose;
    test_sum_ns += test;
    commit_sum_ns += commit;
    total_sum_ns += total;
    if (wait > wait_max_ns)
      wait_max_ns = wait;
    if (compose > compose_max_ns)
      compose_max_ns = compose;
    if (test > test_max_ns)
      test_max_ns = test;
    if (commit > commit_max_ns)
      commit_max_ns = commit;
    if (total > total_max_ns)
      total_max_ns = total;
    ++frames;
  }

  void reset() {
    wait_sum_ns = compose_sum_ns = test_sum_ns = commit_sum_ns = total_sum_ns =
        0;
    wait_max_ns = compose_max_ns = test_max_ns = commit_max_ns = total_max_ns =
        0;
    frames = 0;
  }
};

// Returns true iff `plane_id`'s `rotation` property is a BITMASK that
// includes the `reflect-y` bit. Drivers that don't expose any rotation
// property (zero-rotate planes) or that expose it but list only
// "rotate-0" return false. Called once at init — not on the hot path.
bool PlaneSupportsReflectY(const int fd, const uint32_t plane_id) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return false;
  }
  bool supports = false;
  for (uint32_t i = 0; i < props->count_props && !supports; ++i) {
    auto* prop = drmModeGetProperty(fd, props->props[i]);
    if (prop == nullptr) {
      continue;
    }
    if (std::strcmp(prop->name, "rotation") == 0 &&
        (prop->flags & DRM_MODE_PROP_BITMASK) != 0U) {
      for (int e = 0; e < prop->count_enums; ++e) {
        if (std::strcmp(prop->enums[e].name, "reflect-y") == 0) {
          supports = true;
          break;
        }
      }
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  return supports;
}

// One-shot dump of every property on a DRM object, in "name=value
// [flags]" form. Pre-commit snapshot of the pipe state so we can see
// what fbcon/prior session left on the CRTC / planes / connector.
void DumpObjectProps(const int fd,
                     const uint32_t obj_id,
                     const uint32_t obj_type,
                     const char* label) {
  drmModeObjectPropertiesPtr props =
      drmModeObjectGetProperties(fd, obj_id, obj_type);
  if (props == nullptr) {
    ihs::log::error(
        "[DrmCompositor] snapshot {} id={}: getProperties failed (errno={})",
        label, obj_id, errno);
    return;
  }
  ihs::log::debug("[DrmCompositor] snapshot {} id={} ({} props):", label,
                  obj_id, props->count_props);
  for (uint32_t i = 0; i < props->count_props; ++i) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (prop == nullptr) {
      continue;
    }
    const uint64_t value = props->prop_values[i];
    auto kind = "RANGE";
    if ((prop->flags & DRM_MODE_PROP_IMMUTABLE) != 0U) {
      kind = "IMMUT";
    } else if ((prop->flags & DRM_MODE_PROP_ENUM) != 0U) {
      kind = "ENUM";
    } else if ((prop->flags & DRM_MODE_PROP_BLOB) != 0U) {
      kind = "BLOB";
    } else if ((prop->flags & DRM_MODE_PROP_BITMASK) != 0U) {
      kind = "BITMASK";
    } else if ((prop->flags & DRM_MODE_PROP_OBJECT) != 0U) {
      kind = "OBJECT";
    } else if ((prop->flags & DRM_MODE_PROP_SIGNED_RANGE) != 0U) {
      kind = "SRANGE";
    }
    ihs::log::debug("[DrmCompositor]   {:<20} id={} value={} [{}]", prop->name,
                    prop->prop_id, value, kind);
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
}

const char* PlaneTypeName(const drm::planes::DRMPlaneType t) {
  switch (t) {
    case drm::planes::DRMPlaneType::PRIMARY:
      return "PRIMARY";
    case drm::planes::DRMPlaneType::OVERLAY:
      return "OVERLAY";
    case drm::planes::DRMPlaneType::CURSOR:
      return "CURSOR";
  }
  return "?";
}

}  // namespace

// ─── Lifecycle ───────────────────────────────────────────────────────────

struct DrmCompositor::FrameProfileState {
  FrameProfile p;
};

DrmCompositor::DrmCompositor(DrmBackend* backend, DrmOutputContext out)
    : backend_(backend),
      out_(out),
      profile_(ProfileEnabled() ? std::make_unique<FrameProfileState>()
                                : nullptr) {}

DrmCompositor::~DrmCompositor() {
  (void)WaitForPendingFlip();

  if (texture_blit_fbo_ != 0) {
    glDeleteFramebuffers(1, &texture_blit_fbo_);
  }
  gl_compositor_.reset();

  for (auto& [baton, store] : stores_) {
    DestroyGbmStore(*store);
  }
  for (auto& [baton, store] : stores_) {
    // Copy pointer before deletion so the structured-binding reference
    // (which aliases the map key) does not dangle after the delete.
    const StoreBaton* const ptr = baton;
    delete ptr;
  }
  stores_.clear();

  for (auto& idle : store_pool_) {
    if (idle) {
      DestroyGbmStore(*idle);
    }
  }
  store_pool_.clear();

  for (auto& comp_buf : comp_bufs_) {
    DestroyGbmStore(comp_buf);
  }

  if (bg_store_valid_) {
    DestroyGbmStore(bg_store_);
    bg_store_valid_ = false;
  }
}

// ─── Init helpers ────────────────────────────────────────────────────────

bool DrmCompositor::InitEglExtensions() {
  eglCreateImageKHR_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
      eglGetProcAddress("eglCreateImageKHR"));
  eglDestroyImageKHR_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
      eglGetProcAddress("eglDestroyImageKHR"));
  glEGLImageTargetTexture2DOES_ =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
          eglGetProcAddress("glEGLImageTargetTexture2DOES"));

  // Native-fence sync — used on direct-scanout for IN_FENCE_FD. Probe
  // for the extension on the active EGLDisplay; eglGetProcAddress can
  // return non-null for extensions the display doesn't actually support.
  eglCreateSyncKHR_ = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
      eglGetProcAddress("eglCreateSyncKHR"));
  eglDestroySyncKHR_ = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
      eglGetProcAddress("eglDestroySyncKHR"));
  eglDupNativeFenceFDANDROID_ =
      reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
          eglGetProcAddress("eglDupNativeFenceFDANDROID"));
  const char* exts = eglQueryString(backend_->egl_display(), EGL_EXTENSIONS);
  const bool has_fence_sync =
      exts && std::strstr(exts, "EGL_KHR_fence_sync") != nullptr;
  const bool has_native_fence =
      exts && std::strstr(exts, "EGL_ANDROID_native_fence_sync") != nullptr;
  has_native_fence_sync_ = has_fence_sync && has_native_fence &&
                           eglCreateSyncKHR_ && eglDestroySyncKHR_ &&
                           eglDupNativeFenceFDANDROID_;
  ihs::log::info(
      "[DrmCompositor] explicit sync via IN_FENCE_FD: {} (fence_sync={}, "
      "native_fence_sync={})",
      has_native_fence_sync_ ? "available" : "unavailable",
      has_fence_sync ? "y" : "n", has_native_fence ? "y" : "n");

  return eglCreateImageKHR_ && eglDestroyImageKHR_ &&
         glEGLImageTargetTexture2DOES_;
}

bool DrmCompositor::InitPlaneAllocator() {
  auto reg = drm::planes::PlaneRegistry::enumerate(backend_->device());
  if (!reg) {
    ihs::log::warn("[DrmCompositor] PlaneRegistry: {}", reg.error().message());
    return false;
  }
  plane_registry_.emplace(std::move(*reg));

  const auto available = plane_registry_->for_crtc(out_.crtc_index());
  ihs::log::info(
      "[DrmCompositor] {} planes available for CRTC {} (crtc_index={})",
      available.size(), out_.crtc_id(), out_.crtc_index());
  uint32_t primary_id = 0;
  for (const auto* p : available) {
    auto type_str = "?";
    switch (p->type) {
      case drm::planes::DRMPlaneType::PRIMARY:
        type_str = "PRIMARY";
        break;
      case drm::planes::DRMPlaneType::OVERLAY:
        type_str = "OVERLAY";
        break;
      case drm::planes::DRMPlaneType::CURSOR:
        type_str = "CURSOR";
        break;
    }
    ihs::log::info("[DrmCompositor]   plane {} type={} zpos=[{},{}]", p->id,
                   type_str, p->zpos_min.value_or(-1),
                   p->zpos_max.value_or(-1));
    if (p->type == drm::planes::DRMPlaneType::PRIMARY) {
      // Use zpos_min: when immutable it's the only legal value; when
      // mutable it's still the lowest slot, and we want the root Flutter
      // layer on the bottom.
      primary_zpos_ = p->zpos_min.value_or(0);
      primary_id = static_cast<uint32_t>(p->id);
      // Provisional: whether the primary's rotation enum advertises
      // REFLECT_Y. This is only a hint — vc4/amdgpu DC primaries
      // advertise it but reject it at commit (EINVAL). The non-framed
      // branch below confirms it with a TEST_ONLY commit
      // (ProbePrimaryReflectY) before deciding direct-overlay vs scene.
      primary_supports_reflect_y_ = PlaneSupportsReflectY(
          backend_->drm_fd(), static_cast<uint32_t>(p->id));
    }
    // Per-plane probe for REFLECT_Y. Direct-scanout in PresentLayers
    // sets rotation=REFLECT_Y so GL-rendered (bottom-up) bits flip to
    // scanout (top-down) order. The drm-cxx allocator's static screen
    // filters planes on a coarse supports_rotation (binary) — it
    // doesn't know whether the rotation property's bitmask actually
    // includes reflect-y. Probing each plane here gives us a
    // CRTC-wide "any plane can satisfy us" verdict, which gates the
    // whole direct-scanout attempt. Without that gate, on hardware
    // where NO plane supports REFLECT_Y, we'd burn a TEST_ONLY per
    // plane per frame before giving up.
    if (PlaneSupportsReflectY(backend_->drm_fd(),
                              static_cast<uint32_t>(p->id))) {
      any_plane_supports_reflect_y_ = true;
      ihs::log::info("[DrmCompositor]   plane {} supports REFLECT_Y", p->id);
    }
  }
  ihs::log::info("[DrmCompositor] primary plane zpos = {}", primary_zpos_);

  // rockchip VOP2 advertises REFLECT_Y on its planes and even passes a
  // TEST_ONLY atomic commit with it, but an actual reflected scanout
  // faults the display IOMMU into a storm (rk_iommu "stall request timed
  // out" + vop2 POST_BUF_EMPTY), wedging the pipe — a green/black screen.
  // The TEST_ONLY probe can't catch this (it only validates state, not a
  // live scanout), so blocklist the driver: direct-scanout needs
  // REFLECT_Y to flip GL's bottom-up buffer, so with it gone the
  // compositor uses the GL-composite path (Y-flip baked into the shader,
  // scanned un-reflected) which the VOP scans cleanly.
  if (out_.resolved().driver_name == "rockchip") {
    any_plane_supports_reflect_y_ = false;
    ihs::log::info(
        "[DrmCompositor] rockchip: REFLECT_Y scanout faults the VOP IOMMU; "
        "forcing GL composite");
  }

  // Universal diagnostic kill-switch: force GL composite even when
  // REFLECT_Y is available. Useful for bisecting visual artifacts that
  // appear on the direct-scanout path.
  if (const char* v = std::getenv("IVI_DRM_NO_DIRECT_SCANOUT");
      v && *v && *v != '0') {
    any_plane_supports_reflect_y_ = false;
    ihs::log::info(
        "[DrmCompositor] IVI_DRM_NO_DIRECT_SCANOUT set; forcing GL composite");
  }
  ihs::log::info("[DrmCompositor] direct-scanout REFLECT_Y available: {}",
                 any_plane_supports_reflect_y_ ? "yes" : "no");

  // Pre-commit snapshot: what fbcon/prior session left on the pipe.
  // EINVAL on the first atomic TEST usually correlates to a residual
  // property value (cursor bound to CRTC, stale MODE_ID blob, non-default
  // color encoding, etc.) that we don't overwrite. Dumping once at init
  // lets the next failed-TEST diff against a known starting point.
  if (AllocDebug()) {
    const int fd = backend_->drm_fd();
    DumpObjectProps(fd, out_.crtc_id(), DRM_MODE_OBJECT_CRTC, "CRTC");
    DumpObjectProps(fd, out_.connector_id(), DRM_MODE_OBJECT_CONNECTOR,
                    "Connector");
    for (const auto& p : plane_registry_->all()) {
      char label[32];
      std::snprintf(label, sizeof(label), "Plane[%s]", PlaneTypeName(p.type));
      DumpObjectProps(fd, p.id, DRM_MODE_OBJECT_PLANE, label);
    }
  }

  auto ms = drm::modeset::Modeset::create(backend_->device(), out_.crtc_id(),
                                          out_.connector_id(), out_.mode());
  if (!ms) {
    ihs::log::error("[DrmCompositor] Modeset::create: {}",
                    ms.error().message());
    return false;
  }
  modeset_.emplace(std::move(*ms));

  allocator_ = std::make_unique<drm::planes::Allocator>(backend_->device(),
                                                        *plane_registry_);

  // Per-TEST decorator. On the very first atomic commit the CRTC is
  // still inactive; a plane-only TEST fails with EINVAL because you
  // can't bind an FB to an inactive CRTC. Attaching MODE_ID / ACTIVE /
  // connector.CRTC_ID alongside the plane properties lets the kernel
  // validate the full "power up the pipe + program planes" transaction.
  allocator_->set_test_preparer(
      [this](drm::AtomicRequest& r,
             const uint32_t flags) -> drm::expected<void, std::error_code> {
        if ((flags & DRM_MODE_ATOMIC_ALLOW_MODESET) != 0u && !plane_mode_set_ &&
            modeset_) {
          return modeset_->attach(r);
        }
        return {};
      });

  // Detect framed config (FB smaller than CRTC mode). Allocator won't
  // produce a working commit for this case on drivers that require
  // primary = full CRTC (amdgpu DC); InitFramedMode wires up the
  // dedicated primary-BG + overlay-content path used by PresentFramed.
  framed_ = (out_.width() != out_.mode_width()) ||
            (out_.height() != out_.mode_height());
  if (framed_ && !InitFramedMode()) {
    ihs::log::error(
        "[DrmCompositor] framed-mode init failed; plane path disabled");
    return false;
  }

#if USE_DRM_SCENE
  // The LayerScene drives the non-framed present path, but it only pays
  // off where the bottom-up GL backing store can reach scanout — i.e.
  // some plane supports REFLECT_Y. On hardware where NO plane does
  // (amdgpu DC: primary + overlays all advertise ROTATE_0 only), the
  // scene would set REFLECT_Y on every candidate plane, fail every
  // atomic TEST with EINVAL, and fall back to a slow CompositeCanvas
  // read-back. Skip scene construction entirely there: scene_ stays
  // null and PresentLayers falls through to the legacy GL-composite
  // path (which gates direct-scanout on any_plane_supports_reflect_y_
  // and composites with the orientation flip otherwise). Framed mode
  // keeps its own manual primary-BG + overlay layout.
  if (!framed_ && any_plane_supports_reflect_y_) {
    // Confirm the primary's advertised REFLECT_Y with a TEST_ONLY commit
    // (the enum bitmask false-positives on vc4). If the primary can't
    // actually take REFLECT_Y but an overlay can, use zero-copy
    // direct-overlay mode (opaque BG on the primary, BS on a REFLECT_Y
    // overlay); otherwise drive the scene (direct-scan on the primary).
    primary_supports_reflect_y_ =
        ProbePrimaryReflectY(primary_id, primary_supports_reflect_y_);
    if (!primary_supports_reflect_y_ && InitDirectOverlay()) {
      direct_overlay_mode_ = true;
      // Skip LayerScene construction; PresentDirectOverlay drives the
      // atomic commit manually and never touches scene_.
    } else {
      drm::scene::LayerScene::Config scene_cfg{};
      scene_cfg.crtc_id = out_.crtc_id();
      scene_cfg.connector_id = out_.connector_id();
      scene_cfg.mode = out_.mode();
      auto scene = drm::scene::LayerScene::create(
          const_cast<drm::Device&>(backend_->device()), scene_cfg);
      if (!scene) {
        ihs::log::error("[DrmCompositor] LayerScene::create: {}",
                        scene.error().message());
        return false;
      }
      scene_ = std::move(*scene);
      ApplyCursorReservation();
      ihs::log::info(
          "[DrmCompositor] LayerScene constructed (crtc={} connector={})",
          scene_cfg.crtc_id, scene_cfg.connector_id);
    }
  } else if (!framed_) {
    ihs::log::info(
        "[DrmCompositor] no plane supports REFLECT_Y; using GL-composite "
        "path (scene direct-scanout needs the flip)");
  }
#endif

  return true;
}

bool DrmCompositor::InitFramedMode() {
  // Pick the primary plane (already selected by the allocator's zpos
  // lookup) and the first overlay plane whose zpos range straddles
  // primary_zpos_ + 1 and that advertises the composition buffer's
  // scanout format. The overlay scans out the framed content; the
  // primary scans out a mode-sized opaque BG for the remainder of the
  // CRTC so amdgpu DC's "primary must cover CRTC" check is satisfied.
  const uint32_t comp_format = out_.resolved().primary_format;
  const uint64_t want_overlay_zpos = primary_zpos_ + 1;

  // Overlay planes on a card can be valid for several CRTCs, so a co-tenant
  // view may already own the first one. Skip planes the device-context reports
  // reserved so two views on one card don't collide on a shared overlay.
  const std::vector<uint32_t> reserved =
      backend_->display() != nullptr ? backend_->display()->ReservedPlanes()
                                     : std::vector<uint32_t>{};
  const auto is_reserved = [&reserved](uint32_t id) {
    return std::find(reserved.begin(), reserved.end(), id) != reserved.end();
  };

  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::PRIMARY &&
        framed_primary_id_ == 0) {
      framed_primary_id_ = p->id;
      continue;
    }
    if (p->type == drm::planes::DRMPlaneType::OVERLAY &&
        framed_overlay_id_ == 0) {
      if (!p->supports_format(comp_format) || is_reserved(p->id)) {
        continue;
      }
      // Clamp to the advertised zpos range. When the overlay zpos is
      // immutable (rare), we have to take whatever it reports — the
      // allocator-free commit will still succeed as long as primary
      // and overlay don't land on the same zpos on the same CRTC.
      uint64_t zpos = want_overlay_zpos;
      if (p->zpos_min.has_value() && zpos < *p->zpos_min) {
        zpos = *p->zpos_min;
      }
      if (p->zpos_max.has_value() && zpos > *p->zpos_max) {
        zpos = *p->zpos_max;
      }
      framed_overlay_id_ = p->id;
      framed_overlay_zpos_ = zpos;
    }
  }
  if (framed_primary_id_ == 0 || framed_overlay_id_ == 0) {
    ihs::log::error(
        "[DrmCompositor] framed mode needs 1 primary + 1 overlay plane on "
        "CRTC {} (primary={}, overlay={})",
        out_.crtc_id(), framed_primary_id_, framed_overlay_id_);
    return false;
  }

  // Claim these planes so the next view on this card excludes them.
  if (backend_->display() != nullptr) {
    backend_->display()->ReservePlanes(
        {framed_primary_id_, framed_overlay_id_});
  }

  // Cache property IDs for both planes so PresentFramed can build the
  // atomic request without repeated kernel round-trips.
  const int fd = backend_->drm_fd();
  if (auto r = framed_props_.cache_properties(fd, framed_primary_id_,
                                              DRM_MODE_OBJECT_PLANE);
      !r) {
    ihs::log::error("[DrmCompositor] cache primary props: {}",
                    r.error().message());
    return false;
  }
  if (auto r = framed_props_.cache_properties(fd, framed_overlay_id_,
                                              DRM_MODE_OBJECT_PLANE);
      !r) {
    ihs::log::error("[DrmCompositor] cache overlay props: {}",
                    r.error().message());
    return false;
  }
  // Also, cache non-cursor planes we'll need to disable each frame, so
  // their FB_ID/CRTC_ID IDs are resolvable without touching the kernel.
  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    if (p->id == framed_primary_id_ || p->id == framed_overlay_id_) {
      continue;
    }
    (void)framed_props_.cache_properties(fd, p->id, DRM_MODE_OBJECT_PLANE);
  }

  ihs::log::info(
      "[DrmCompositor] framed mode: primary={} overlay={} (zpos={}), "
      "BG {}x{}, content {}x{} at ({},{})",
      framed_primary_id_, framed_overlay_id_, framed_overlay_zpos_,
      out_.mode_width(), out_.mode_height(), out_.width(), out_.height(),
      (out_.mode_width() - out_.width()) / 2,
      (out_.mode_height() - out_.height()) / 2);
  return true;
}

bool DrmCompositor::InitCompositionBuffers() {
  // Composition buffer scans out on the primary plane, so its GBM format
  // must match what the primary accepts — use the resolved format, not a
  // hardcoded constant. (XRGB on most drivers, XBGR on e.g. tilcdc.)
  const uint32_t format = out_.resolved().primary_format;
  for (auto& comp_buf : comp_bufs_) {
    if (!CreateGbmStore(comp_buf, out_.width(), out_.height(), format) ||
        !EnsureDrmFbId(comp_buf)) {
      return false;
    }
  }
  comp_bufs_valid_ = true;

  if (framed_) {
    // Mode-sized opaque BG pinned to the primary plane for the session.
    // Filled with black once via GL; contents never change — the overlay
    // plane carries all moving pixels. Created here (after comp_bufs_)
    // so the same GL context used for composition is current.
    if (!CreateGbmStore(bg_store_, out_.mode_width(), out_.mode_height(),
                        format) ||
        !EnsureDrmFbId(bg_store_)) {
      ihs::log::error("[DrmCompositor] BG GBM store create failed");
      return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, bg_store_.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();  // commit BG pixels before the first atomic scanout binds it
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bg_store_valid_ = true;
  }
  return true;
}

void DrmCompositor::EnsureGlCapsProbed() {
  if (gl_caps_probed_) {
    return;
  }
  gl_caps_.Probe();
  gl_compositor_ = std::make_unique<GlCompositor>(&gl_caps_);
  if (!InitEglExtensions()) {
    ihs::log::error("[DrmCompositor] missing EGL_KHR_image / GL_OES_EGL_image");
  }

  // Only take the plane path if DriverProbe said this driver supports it
  // (atomic + primary + ≥1 overlay). Otherwise, all frames go through
  // PresentViaGlFallback — same outcome as before, just honest about it.
  if (out_.resolved().use_plane_compositor) {
    if (!InitPlaneAllocator()) {
      ihs::log::warn(
          "[DrmCompositor] plane allocator init failed; GL fallback");
    } else {
      // Flip planes_available_ BEFORE InitCompositionBuffers so the
      // CreateGbmStore call-chain inside it imports each comp BO as a
      // KMS FB. Otherwise, comp.drm_fb_id is 0, and the atomic commit
      // tells the kernel to disable the primary plane — blank screen.
      planes_available_ = true;
      if (InitCompositionBuffers()) {
        ihs::log::info("[DrmCompositor] plane allocator active");
      } else {
        planes_available_ = false;
        ihs::log::warn(
            "[DrmCompositor] composition buffer init failed; GL fallback");
      }
    }
  } else {
    ihs::log::info(
        "[DrmCompositor] plane compositor disabled by probe; GL path");
  }
  gl_caps_probed_ = true;
}

// ─── GBM store create / destroy ──────────────────────────────────────────

bool DrmCompositor::CreateGbmStore(GbmBackingStore& store,
                                   uint32_t w,
                                   uint32_t h,
                                   const uint32_t format,
                                   const size_t pool_size,
                                   const bool force_scanout) const {
  if (pool_size == 0 || pool_size > GbmBackingStore::kMaxPoolSize) {
    ihs::log::error(
        "[DrmCompositor] CreateGbmStore: pool_size={} out of range [1, {}]",
        pool_size, GbmBackingStore::kMaxPoolSize);
    return false;
  }
  store.width = w;
  store.height = h;
  store.format = format;
  store.pool_size = pool_size;
  const uint32_t usage = (planes_available_ || force_scanout)
                             ? (GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT)
                             : GBM_BO_USE_RENDERING;

  // Allocate N (= pool_size) gbm_bos + EGLImages + color textures, one
  // per pool slot. Shared FBO + depth/stencil RB are created once at the
  // end and attached to slot 0 initially; PresentLayers rebinds the
  // color attachment as the pool rotates. On any per-slot failure,
  // DestroyGbmStore handles partial state (it tolerates zero-value
  // fields in each slot).
  for (size_t i = 0; i < pool_size; ++i) {
    auto& slot = store.pool[i];
    slot.bo = gbm_bo_create(backend_->gbm(), w, h, format, usage);
    if (!slot.bo) {
      ihs::log::error("[DrmCompositor] gbm_bo_create slot={} {}x{}: {}", i, w,
                      h, std::strerror(errno));
      DestroyGbmStore(store);
      return false;
    }

    // Import the gbm_bo via EGL_EXT_image_dma_buf_import. Mesa's
    // EGL_NATIVE_PIXMAP_KHR-over-gbm_bo convention is not implemented
    // by NVIDIA's EGL (returns EGL_BAD_PARAMETER on L4T). The dmabuf
    // path is portable across amdgpu / Intel / Mali / NVIDIA. Modifier
    // attribs are appended when the bo carries a real modifier —
    // required by NVIDIA, harmless to Mesa.
    const int dmabuf_fd = gbm_bo_get_fd(slot.bo);
    if (dmabuf_fd < 0) {
      ihs::log::error("[DrmCompositor] gbm_bo_get_fd slot={}: {}", i,
                      std::strerror(errno));
      DestroyGbmStore(store);
      return false;
    }
    const uint32_t stride = gbm_bo_get_stride_for_plane(slot.bo, 0);
    const uint32_t offset = gbm_bo_get_offset(slot.bo, 0);
    const uint64_t modifier = gbm_bo_get_modifier(slot.bo);

    // eglCreateImageKHR's attrib_list is EGLint*, not EGLAttrib*;
    // modifier halves are passed as the bit-pattern of each 32-bit half.
    std::array<EGLint, 21> attribs{};
    size_t n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = static_cast<EGLint>(w);
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = static_cast<EGLint>(h);
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = static_cast<EGLint>(format);
    attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attribs[n++] = dmabuf_fd;
    attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attribs[n++] = static_cast<EGLint>(offset);
    attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attribs[n++] = static_cast<EGLint>(stride);
    if (modifier != DRM_FORMAT_MOD_INVALID) {
      attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
      attribs[n++] = static_cast<EGLint>(modifier & 0xffffffffULL);
      attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
      attribs[n++] = static_cast<EGLint>(modifier >> 32);
    }
    attribs[n++] = EGL_NONE;

    slot.egl_image = eglCreateImageKHR_(
        backend_->egl_display(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
        static_cast<EGLClientBuffer>(nullptr), attribs.data());
    close(dmabuf_fd);  // EGL dup's the fd; safe to close after the call.
    if (slot.egl_image == EGL_NO_IMAGE_KHR) {
      ihs::log::error(
          "[DrmCompositor] eglCreateImageKHR(slot={}, LINUX_DMA_BUF, "
          "fourcc={}, stride={}, offset={}, mod=0x{:x}): 0x{:x}",
          i, drm::format_name(format), stride, offset, modifier, eglGetError());
      DestroyGbmStore(store);
      return false;
    }

    glGenTextures(1, &slot.color_tex);
    glBindTexture(GL_TEXTURE_2D, slot.color_tex);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D,
                                  static_cast<GLeglImageOES>(slot.egl_image));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  // Shared FBO + depth/stencil RB. Color attachment initially points
  // at slot 0; PresentLayers rebinds it as the pool rotates.
  store.active_idx = 0;
  store.pool[0].state = GbmBackingStore::Slot::State::Active;

  glGenFramebuffers(1, &store.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, store.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         store.pool[0].color_tex, 0);

  glGenRenderbuffers(1, &store.depth_stencil_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, store.depth_stencil_rb);
  if (gl_caps_.has_packed_depth_stencil) {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES,
                          static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, store.depth_stencil_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, store.depth_stencil_rb);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, store.depth_stencil_rb);
  }

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    ihs::log::error("[DrmCompositor] FBO incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    DestroyGbmStore(store);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // KMS framebuffer import is deferred to EnsureDrmFbId(). Callers that
  // immediately scan out this BO (comp_bufs_, bg_store_) must invoke
  // EnsureDrmFbId() after construction; Flutter-side backing stores
  // let the layer-setup path import on the first direct-scanout use.
  return true;
}

bool DrmCompositor::EnsureDrmFbId(GbmBackingStore& store) const {
  auto& slot = store.active();
  if (slot.drm_fb_id != 0) {
    return true;
  }
  if (!slot.bo) {
    return false;
  }
  slot.drm_fb_id = ImportBoAsFb(slot.bo);
  return slot.drm_fb_id != 0;
}

uint32_t DrmCompositor::ImportBoAsFb(gbm_bo* bo) const {
  const uint32_t w = gbm_bo_get_width(bo);
  const uint32_t h = gbm_bo_get_height(bo);
  const uint32_t format = gbm_bo_get_format(bo);
  const uint64_t modifier = gbm_bo_get_modifier(bo);
  const int plane_count = gbm_bo_get_plane_count(bo);

  uint32_t handles[4] = {};
  uint32_t pitches[4] = {};
  uint32_t offsets[4] = {};
  uint64_t modifiers[4] = {};
  for (int i = 0; i < plane_count && i < 4; ++i) {
    handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
    pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);
    modifiers[i] = modifier;
  }

  uint32_t fb_id = 0;
  const bool use_modifiers = (modifier != DRM_FORMAT_MOD_INVALID) &&
                             (modifier != DRM_FORMAT_MOD_LINEAR);

  if (use_modifiers) {
    if (drmModeAddFB2WithModifiers(backend_->drm_fd(), w, h, format, handles,
                                   pitches, offsets, modifiers, &fb_id,
                                   DRM_MODE_FB_MODIFIERS) != 0) {
      ihs::log::error(
          "[DrmCompositor] drmModeAddFB2WithModifiers({}x{}, fmt=0x{:08x}, "
          "mod=0x{:016x}, planes={}): {}",
          w, h, format, modifier, plane_count, std::strerror(errno));
      return 0;
    }
    if (backend_->cfg_.debug_backend) {
      ihs::log::debug(
          "[DrmCompositor] AddFB2WithModifiers fb_id={} {}x{} fmt=0x{:08x} "
          "mod=0x{:016x} planes={}",
          fb_id, w, h, format, modifier, plane_count);
    }
  } else {
    if (drmModeAddFB2(backend_->drm_fd(), w, h, format, handles, pitches,
                      offsets, &fb_id, 0) != 0) {
      ihs::log::error(
          "[DrmCompositor] drmModeAddFB2({}x{}, fmt=0x{:08x}, linear): {}", w,
          h, format, std::strerror(errno));
      return 0;
    }
    if (backend_->cfg_.debug_backend) {
      ihs::log::debug(
          "[DrmCompositor] AddFB2 fb_id={} {}x{} fmt=0x{:08x} linear", fb_id, w,
          h, format);
    }
  }
  return fb_id;
}

void DrmCompositor::DestroyGbmStore(GbmBackingStore& store) const {
  // Shared resources first: one FBO + one depth/stencil RB per BS.
  if (store.fbo != 0) {
    glDeleteFramebuffers(1, &store.fbo);
    store.fbo = 0;
  }
  if (store.depth_stencil_rb != 0) {
    glDeleteRenderbuffers(1, &store.depth_stencil_rb);
    store.depth_stencil_rb = 0;
  }
  // Per-slot resources.
  for (auto& slot : store.pool) {
    if (slot.color_tex != 0) {
      glDeleteTextures(1, &slot.color_tex);
      slot.color_tex = 0;
    }
    if (slot.egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_) {
      eglDestroyImageKHR_(backend_->egl_display(), slot.egl_image);
      slot.egl_image = EGL_NO_IMAGE_KHR;
    }
    if (slot.drm_fb_id != 0) {
      drmModeRmFB(backend_->drm_fd(), slot.drm_fb_id);
      slot.drm_fb_id = 0;
    }
    if (slot.bo) {
      gbm_bo_destroy(slot.bo);
      slot.bo = nullptr;
    }
    slot.state = GbmBackingStore::Slot::State::Free;
  }
  store.active_idx = 0;
}

// ─── Page-flip synchronization ───────────────────────────────────────────

void DrmCompositor::OnFlipEvent(const unsigned int tv_sec,
                                const unsigned int tv_usec) {
  OnFlipComplete();
  // Only the vsync-driving (primary) output returns the baton; additional
  // outputs present off the same engine tick and must not double-deliver.
  if (drives_vsync_) {
    backend_->DeliverVsyncFromFlip(tv_sec, tv_usec);
  }
}

void DrmCompositor::OnFlipComplete() {
  // Called from DrmBackend::UnifiedPageFlipHandler on the platform
  // task runner thread when planes are active. Baton return is the
  // unified handler's job; we just record the flip.
  flip_pending_.store(false, std::memory_order_release);

  // Advance the BS-slot release pipeline. The slots that *were*
  // scanning out are now displaced and free; the slots from the most
  // recent commit (front of in_flight_slots_) become the new scanning
  // set. The event we just received is semantically "previous front
  // retired, new front is now active", which is exactly this
  // transition.
  {
    std::lock_guard<std::mutex> lock(slot_pipeline_mu_);
    for (auto& ref : scanning_slots_) {
      ref.store->pool[ref.slot_idx].state = GbmBackingStore::Slot::State::Free;
    }
    scanning_slots_.clear();
    if (!in_flight_slots_.empty()) {
      scanning_slots_ = std::move(in_flight_slots_.front());
      in_flight_slots_.pop_front();
    }
  }

  backend_->RecordFlipComplete();
}

bool DrmCompositor::WaitForPendingFlip() const {
  // The card's flip reader (owned by DrmDisplay, on its own thread) is the
  // single reader of the shared fd: it drains PAGE_FLIP_EVENTs and clears
  // flip_pending_ via UnifiedPageFlipHandler → OnFlipComplete. We only wait for
  // the flag -- we must NOT read the fd here (a second reader would race the
  // reader thread). Waiting on the flag is also what makes a merged
  // raster+platform thread safe: an independent thread delivers the completion,
  // so blocking here can never deadlock the thread that must drain it.
  using namespace std::chrono_literals;
  // Bounded wait: on nvidia-drm a flip event can be lost; without a cap the
  // destructor's wait spins forever and the process never exits on SIGTERM. A
  // healthy 60Hz flip clears in ~16ms so the cap never fires in normal
  // operation.
  constexpr auto kFlipWaitTimeout = 100ms;
  const auto deadline = std::chrono::steady_clock::now() + kFlipWaitTimeout;
  while (flip_pending_.load(std::memory_order_acquire)) {
    // Session went inactive between submitting the flip and the kernel firing
    // PAGE_FLIP_EVENT — the event will never come. OnResume() clears
    // flip_pending_; bail now so the rasterizer doesn't spin forever.
    if (paused_.load(std::memory_order_acquire)) {
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ihs::log::warn(
          "[DrmCompositor] WaitForPendingFlip: no flip completion after "
          "100ms; proceeding (PAGE_FLIP_EVENT likely lost)");
      return true;
    }
    std::this_thread::sleep_for(200us);
  }
  return true;
}

// ─── GL compositing helpers ──────────────────────────────────────────────

void DrmCompositor::CompositeLayerIntoFbo(GLuint target_fbo,
                                          GLuint src_fbo,
                                          GLuint src_tex,
                                          GLsizei src_w,
                                          GLsizei src_h,
                                          GLint dst_x,
                                          GLint dst_y,
                                          GLsizei dst_w,
                                          GLsizei dst_h,
                                          bool blend,
                                          bool flip_y) const {
  gl_compositor_->CompositeToFbo(target_fbo, src_fbo, src_tex, src_w, src_h,
                                 dst_x, dst_y, dst_w, dst_h, blend, flip_y);
}

// ─── GL fallback (no plane allocator) ────────────────────────────────────

bool DrmCompositor::PresentViaGlFallback(const FlutterLayer** layers,
                                         const size_t count) {
  // The GL fallback composites into, and page-flips, the backend's own
  // (primary) gbm_surface/CRTC via DrmBackend::Present(). A secondary output
  // must not touch that -- doing so would render its frame onto the
  // primary display's CRTC and never clear its own flip. A per-output
  // GL-fallback surface is a follow-up; until then drop the frame on secondary
  // outputs rather than corrupt the primary. The zero-copy scene path (the
  // normal zero-copy scene path) does not reach this.
  if (!drives_vsync_) {
    ihs::log::warn(
        "[DrmCompositor] GL fallback unavailable on a secondary output "
        "(crtc={}); dropping frame",
        out_.crtc_id());
    return true;  // ack to Flutter; nothing presented, primary untouched
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  bool composited_any = false;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    const bool blend = composited_any;
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      auto* baton = static_cast<StoreBaton*>(layer->backing_store->user_data);
      if (baton && baton->store) {
        const auto* s = baton->store;
        gl_compositor_->CompositeToDefault(
            s->fbo, s->active().color_tex, static_cast<GLsizei>(s->width),
            static_cast<GLsizei>(s->height),
            static_cast<GLint>(layer->offset.x),
            static_cast<GLint>(layer->offset.y),
            static_cast<GLsizei>(layer->size.width),
            static_cast<GLsizei>(layer->size.height), blend);
        composited_any = true;
      }
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      std::shared_ptr<ICompositorSurface> surface_sp;
      {
        std::lock_guard<std::mutex> lock(surfaces_mu_);
        if (auto it = surfaces_.find(layer->platform_view->identifier);
            it != surfaces_.end()) {
          surface_sp = it->second;
        }
      }
      if (surface_sp) {
        surface_sp->OnPresent(layer);
        if (const auto tex = surface_sp->GetGlTextureName(); tex != 0) {
          const auto sw = surface_sp->GetGlTextureWidth();
          const auto sh = surface_sp->GetGlTextureHeight();
          const auto dx = static_cast<GLint>(layer->offset.x);
          const auto dw = static_cast<GLsizei>(layer->size.width);
          const auto dh = static_cast<GLsizei>(layer->size.height);
          const auto dy = static_cast<GLint>(out_.height()) -
                          static_cast<GLint>(layer->offset.y) - dh;
          // GlFallback writes into FBO 0 (gbm_surface, standard GL NDC).
          // Flip only when the surface reports top-first storage.
          const bool flip_y = surface_sp->TextureIsTopFirst();
          gl_compositor_->CompositeToDefault(0, tex, sw, sh, dx, dy, dw, dh,
                                             blend, flip_y);
          composited_any = true;
        }
      }
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return backend_->Present();
}

// ─── Cursor staging + shared commit settle ──────────────────────────────

bool DrmCompositor::StageCursorInto(drm::AtomicRequest& req) {
  bool needs_modeset = false;
  if (cursor_ != nullptr) {
    (void)cursor_->Stage(req, &needs_modeset);
  }
  return needs_modeset;
}

void DrmCompositor::SettleAtomicCommit(const bool blocking) {
  if (blocking) {
    // A blocking commit (initial modeset or a cursor first-enable) lands
    // synchronously with no PAGE_FLIP_EVENT. The genuine first modeset
    // also latches plane_mode_set_ and verifies the pipe. Either way the
    // baton Flutter requested while we built this frame must be returned
    // inline (the asio flip path won't fire). DeliverParkedBaton marshals via
    // the platform task runner (we're on the rasterizer thread).
    if (!plane_mode_set_) {
      plane_mode_set_ = true;
      VerifyPipeRunning();
    }
    flip_pending_.store(false, std::memory_order_release);
    (void)backend_->vsync_.DeliverParkedBaton();
  } else {
    flip_pending_.store(true, std::memory_order_release);
  }
}

// ─── Framed-mode present (primary BG + overlay content) ─────────────────

bool DrmCompositor::PresentFramed(const FlutterLayer** layers,
                                  const size_t count) {
  // Frame profile fence posts: t0 entry → t1 post-wait → t2 post-compose
  // → t3 post-TEST_ONLY → t4 post-commit. All in monotonic ns. Cheap
  // enough (~30 ns per call) to leave unconditional; only the bookkeep
  // + summary log are gated on IVI_DRM_PROFILE.
  const bool profile = ProfileEnabled();
  const uint64_t t0 = profile ? NsNow() : 0;

  if (!WaitForPendingFlip()) {
    return PresentViaGlFallback(layers, count);
  }
  const uint64_t t1 = profile ? NsNow() : 0;

  // Composite every Flutter layer into the current comp buffer. Same
  // pixel work as PresentViaGlFallback, but the destination is the GBM
  // BO we'll scan out on the overlay rather than FBO 0.
  auto& comp = comp_bufs_[comp_idx_];
  glBindFramebuffer(GL_FRAMEBUFFER, comp.fbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  struct PvProbe {
    size_t layer_idx;
    int64_t id;
    GLint cx;
    GLint cy;
    std::array<uint8_t, 4> after_pv;
  };
  std::vector<PvProbe> pv_probes;

  auto read_px = [&](GLint x, GLint y) -> std::array<uint8_t, 4> {
    std::array<uint8_t, 4> px{};
    glBindFramebuffer(GL_FRAMEBUFFER, comp.fbo);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
  };

  bool composited_any = false;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    const bool blend = composited_any;
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      auto* baton = static_cast<StoreBaton*>(layer->backing_store->user_data);
      if (baton && baton->store) {
        const auto* s = baton->store;
        if (backend_->cfg_.debug_backend) {
          ihs::log::debug(
              "[DrmCompositor] framed layer[{}] BS src={}x{} "
              "offset=({:.1f},{:.1f}) size={:.1f}x{:.1f} blend={}",
              i, s->width, s->height, layer->offset.x, layer->offset.y,
              layer->size.width, layer->size.height, blend);
        }
        CompositeLayerIntoFbo(comp.fbo, s->fbo, s->active().color_tex,
                              static_cast<GLsizei>(s->width),
                              static_cast<GLsizei>(s->height),
                              static_cast<GLint>(layer->offset.x),
                              static_cast<GLint>(layer->offset.y),
                              static_cast<GLsizei>(layer->size.width),
                              static_cast<GLsizei>(layer->size.height), blend,
                              /*flip_y=*/true);
        composited_any = true;
        if (backend_->cfg_.debug_backend && !pv_probes.empty()) {
          for (const auto& p : pv_probes) {
            auto px = read_px(p.cx, p.cy);
            ihs::log::debug(
                "[DrmCompositor] probe PV id={} @({},{}) after BS layer[{}] "
                "rgba=({},{},{},{})",
                p.id, p.cx, p.cy, i, px[0], px[1], px[2], px[3]);
          }
        }
      } else if (backend_->cfg_.debug_backend) {
        ihs::log::debug(
            "[DrmCompositor] framed layer[{}] BS SKIPPED (baton/store null)",
            i);
      }
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      std::shared_ptr<ICompositorSurface> surface_sp;
      size_t surfaces_size = 0;
      {
        std::lock_guard<std::mutex> lock(surfaces_mu_);
        surfaces_size = surfaces_.size();
        if (auto it = surfaces_.find(layer->platform_view->identifier);
            it != surfaces_.end()) {
          surface_sp = it->second;
        }
      }
      if (!surface_sp) {
        if (backend_->cfg_.debug_backend) {
          ihs::log::debug(
              "[DrmCompositor] framed layer[{}] PV id={} SKIPPED (no "
              "registered surface; registry size={})",
              i, layer->platform_view->identifier, surfaces_size);
        }
      } else {
        surface_sp->OnPresent(layer);
        const auto tex = surface_sp->GetGlTextureName();
        if (tex == 0) {
          if (backend_->cfg_.debug_backend) {
            ihs::log::debug(
                "[DrmCompositor] framed layer[{}] PV id={} SKIPPED "
                "(GetGlTextureName=0)",
                i, layer->platform_view->identifier);
          }
        } else {
          // comp.fbo has KMS-inverted NDC-y vs. GL's window convention.
          // GL-native (bottom-first) sources need flip_y=true; top-first
          // sources (e.g. NV12 dmabuf or pre-flipped YUV shader) need
          // flip_y=false so their already-top-down bytes land right-side-up.
          const bool flip_y = !surface_sp->TextureIsTopFirst();
          if (backend_->cfg_.debug_backend) {
            ihs::log::debug(
                "[DrmCompositor] framed layer[{}] PV id={} tex={} "
                "src={}x{} offset=({:.1f},{:.1f}) size={:.1f}x{:.1f} "
                "blend={} flip_y={} top_first={}",
                i, layer->platform_view->identifier, tex,
                surface_sp->GetGlTextureWidth(),
                surface_sp->GetGlTextureHeight(), layer->offset.x,
                layer->offset.y, layer->size.width, layer->size.height, blend,
                flip_y, surface_sp->TextureIsTopFirst());
          }
          CompositeLayerIntoFbo(
              comp.fbo, /*src_fbo=*/0, tex, surface_sp->GetGlTextureWidth(),
              surface_sp->GetGlTextureHeight(),
              static_cast<GLint>(layer->offset.x),
              static_cast<GLint>(layer->offset.y),
              static_cast<GLsizei>(layer->size.width),
              static_cast<GLsizei>(layer->size.height), blend, flip_y);
          composited_any = true;
          if (backend_->cfg_.debug_backend) {
            const auto cx =
                static_cast<GLint>(layer->offset.x + layer->size.width / 2.0);
            const auto cy =
                static_cast<GLint>(layer->offset.y + layer->size.height / 2.0);
            auto px = read_px(cx, cy);
            ihs::log::debug(
                "[DrmCompositor] probe PV id={} @({},{}) after-PV "
                "rgba=({},{},{},{})",
                layer->platform_view->identifier, cx, cy, px[0], px[1], px[2],
                px[3]);
            pv_probes.push_back(
                {i, layer->platform_view->identifier, cx, cy, px});
          }
        }
      }
    } else if (backend_->cfg_.debug_backend) {
      ihs::log::debug("[DrmCompositor] framed layer[{}] unhandled type={}", i,
                      static_cast<int>(layer->type));
    }
  }
  if (backend_->cfg_.debug_backend && !pv_probes.empty()) {
    for (const auto& p : pv_probes) {
      auto px = read_px(p.cx, p.cy);
      const bool changed = px != p.after_pv;
      ihs::log::debug(
          "[DrmCompositor] probe PV id={} @({},{}) after-all "
          "rgba=({},{},{},{}) was=({},{},{},{}) {}",
          p.id, p.cx, p.cy, px[0], px[1], px[2], px[3], p.after_pv[0],
          p.after_pv[1], p.after_pv[2], p.after_pv[3],
          changed ? "(OVERWRITTEN)" : "(unchanged)");
    }
  }
  glFinish();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (backend_->cfg_.debug_backend) {
    ihs::log::debug(
        "[DrmCompositor] framed frame: layers={} composited={} comp_idx={} "
        "comp_fb={}",
        count, composited_any, comp_idx_, comp.active().drm_fb_id);
  }

  // ── Build the atomic request ──

  drm::AtomicRequest req(backend_->device());
  if (!req.valid()) {
    ihs::log::warn("[DrmCompositor] framed: AtomicRequest alloc failed");
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, count);
  }

  const uint32_t crtc_id = out_.crtc_id();
  const uint32_t mode_w = out_.mode_width();
  const uint32_t mode_h = out_.mode_height();
  const uint32_t fb_w = out_.width();
  const uint32_t fb_h = out_.height();
  const int32_t lx = static_cast<int32_t>(mode_w - fb_w) / 2;
  const int32_t ly = static_cast<int32_t>(mode_h - fb_h) / 2;

  const bool dump = backend_->cfg_.debug_backend && !plane_mode_set_;
  if (dump) {
    ihs::log::debug(
        "[DrmCompositor] framed commit: crtc={} primary={} overlay={} "
        "mode={}x{} fb={}x{} offset=({},{})",
        crtc_id, framed_primary_id_, framed_overlay_id_, mode_w, mode_h, fb_w,
        fb_h, lx, ly);
  }

  // First commit only: attach MODE_ID / ACTIVE / Connector.CRTC_ID so
  // the kernel actually powers up the pipe. Plane writes alone don't
  // activate the CRTC.
  if (!plane_mode_set_ && modeset_) {
    if (auto r = modeset_->attach(req); !r) {
      ihs::log::warn("[DrmCompositor] framed: modeset attach: {}",
                     r.error().message());
      return PresentViaGlFallback(layers, count);
    }
    if (dump) {
      ihs::log::debug(
          "[DrmCompositor] framed commit: modeset_->attach() done "
          "(CRTC.ACTIVE/MODE_ID + connector.CRTC_ID)");
    }
  }

  // Helper: write one property or bail into the GL fallback. Property
  // lookups hit the in-memory cache populated in InitFramedMode, so
  // this is fast. When `dump` is set (first commit + debug_backend), also
  // log each write so a failed commit can be post-mortemed without
  // re-running under strace.
  auto set = [&](const uint32_t obj, const char* name,
                 const uint64_t value) -> bool {
    auto pid = framed_props_.property_id(obj, name);
    if (!pid) {
      ihs::log::warn("[DrmCompositor] framed: property {}.{} missing: {}", obj,
                     name, pid.error().message());
      return false;
    }
    if (auto r = req.add_property(obj, *pid, value); !r) {
      ihs::log::warn("[DrmCompositor] framed: add_property {}.{}: {}", obj,
                     name, r.error().message());
      return false;
    }
    if (dump) {
      ihs::log::debug("[DrmCompositor] framed prop: obj={} pid={} {}={}", obj,
                      *pid, name, value);
    }
    return true;
  };

  auto log_raw = [&](const uint32_t obj, const uint32_t pid, const char* name,
                     const uint64_t value) {
    if (dump) {
      ihs::log::debug("[DrmCompositor] framed prop: obj={} pid={} {}={}", obj,
                      pid, name, value);
    }
  };

  // Primary: persistent mode-sized opaque BG covering the whole CRTC.
  // Kept on every frame, so amdgpu DC sees "primary covers CRTC".
  if (!set(framed_primary_id_, "FB_ID", bg_store_.active().drm_fb_id) ||
      !set(framed_primary_id_, "CRTC_ID", crtc_id) ||
      !set(framed_primary_id_, "CRTC_X", 0) ||
      !set(framed_primary_id_, "CRTC_Y", 0) ||
      !set(framed_primary_id_, "CRTC_W", mode_w) ||
      !set(framed_primary_id_, "CRTC_H", mode_h) ||
      !set(framed_primary_id_, "SRC_X", 0) ||
      !set(framed_primary_id_, "SRC_Y", 0) ||
      !set(framed_primary_id_, "SRC_W", static_cast<uint64_t>(mode_w) << 16) ||
      !set(framed_primary_id_, "SRC_H", static_cast<uint64_t>(mode_h) << 16)) {
    return PresentViaGlFallback(layers, count);
  }
  // zpos may be immutable on some drivers (amdgpu primary = [2,2]).
  // The kernel rejects any atomic write to an IMMUTABLE property with
  // -EINVAL regardless of value, so skip the write when the cached flags
  // say it's immutable. Missing property is benign.
  if (auto pid = framed_props_.property_id(framed_primary_id_, "zpos")) {
    if (const auto immut =
            framed_props_.is_immutable(framed_primary_id_, "zpos");
        !immut || !*immut) {
      (void)req.add_property(framed_primary_id_, *pid, primary_zpos_);
      log_raw(framed_primary_id_, *pid, "zpos", primary_zpos_);
    }
  }

  // Overlay: carries the composited content, centered on the CRTC.
  if (!set(framed_overlay_id_, "FB_ID", comp.active().drm_fb_id) ||
      !set(framed_overlay_id_, "CRTC_ID", crtc_id) ||
      !set(framed_overlay_id_, "CRTC_X",
           static_cast<uint64_t>(static_cast<int64_t>(lx))) ||
      !set(framed_overlay_id_, "CRTC_Y",
           static_cast<uint64_t>(static_cast<int64_t>(ly))) ||
      !set(framed_overlay_id_, "CRTC_W", fb_w) ||
      !set(framed_overlay_id_, "CRTC_H", fb_h) ||
      !set(framed_overlay_id_, "SRC_X", 0) ||
      !set(framed_overlay_id_, "SRC_Y", 0) ||
      !set(framed_overlay_id_, "SRC_W", static_cast<uint64_t>(fb_w) << 16) ||
      !set(framed_overlay_id_, "SRC_H", static_cast<uint64_t>(fb_h) << 16)) {
    return PresentViaGlFallback(layers, count);
  }
  if (auto pid = framed_props_.property_id(framed_overlay_id_, "zpos")) {
    if (const auto immut =
            framed_props_.is_immutable(framed_overlay_id_, "zpos");
        !immut || !*immut) {
      (void)req.add_property(framed_overlay_id_, *pid, framed_overlay_zpos_);
      log_raw(framed_overlay_id_, *pid, "zpos", framed_overlay_zpos_);
    }
  }

  // Disable every other non-cursor plane on this CRTC so the commit
  // doesn't inherit stale FB/CRTC/zpos from fbcon or a prior session --
  // except planes another view on this card owns. Shared overlays are valid
  // for our CRTC too, so disabling one would blank the co-tenant view. Checked
  // at commit (both views have reserved by steady state; a co-tenant that has
  // not reserved yet is not scanning out, so disabling it is harmless).
  const std::vector<uint32_t> reserved =
      backend_->display() != nullptr ? backend_->display()->ReservedPlanes()
                                     : std::vector<uint32_t>{};
  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    if (p->id == framed_primary_id_ || p->id == framed_overlay_id_) {
      continue;
    }
    if (std::find(reserved.begin(), reserved.end(), p->id) != reserved.end()) {
      continue;
    }
    if (auto fb_pid = framed_props_.property_id(p->id, "FB_ID")) {
      (void)req.add_property(p->id, *fb_pid, 0);
      log_raw(p->id, *fb_pid, "FB_ID(disable)", 0);
    }
    if (auto crtc_pid = framed_props_.property_id(p->id, "CRTC_ID")) {
      (void)req.add_property(p->id, *crtc_pid, 0);
      log_raw(p->id, *crtc_pid, "CRTC_ID(disable)", 0);
    }
  }

  // ── Commit ──

  if (backend_->cfg_.debug_backend) {
    backend_->flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }

  // Stage the HW cursor into this commit so it rides the same atomic flip
  // instead of issuing its own (a separate cursor commit on this CRTC
  // starves the compositor's PAGE_FLIP_EVENT on nvidia-drm). The cursor's
  // first plane-enable needs ALLOW_MODESET, which can't be NONBLOCK on
  // most drivers, so fold it into a blocking commit for that one frame —
  // exactly how the initial modeset is handled.
  const bool blocking = !plane_mode_set_ || StageCursorInto(req);
  const uint32_t commit_flags =
      blocking ? DRM_MODE_ATOMIC_ALLOW_MODESET
               : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);

  // Profile fence post: end of compose / req build, just before the
  // optional TEST_ONLY probe + the real commit.
  const uint64_t t2 = profile ? NsNow() : 0;

  // Debug-only TEST_ONLY probe on the very first commit so a failed
  // real commit can be attributed to validation (EINVAL returned here)
  // vs. page-flip/driver dispatch. No side effect if the test succeeds.
  if (dump) {
    constexpr uint32_t test_flags =
        DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (auto r = req.test(test_flags); !r) {
      ihs::log::warn(
          "[DrmCompositor] framed TEST_ONLY commit failed ({}); real commit "
          "will likely fail with the same error",
          r.error().message());
    } else {
      ihs::log::debug("[DrmCompositor] framed TEST_ONLY commit passed");
    }
  }
  const uint64_t t3 = profile ? NsNow() : 0;

  if (dump) {
    ihs::log::debug("[DrmCompositor] framed commit: flags=0x{:x}",
                    commit_flags);
  }

  if (auto r = req.commit(commit_flags, static_cast<IFlipSink*>(this)); !r) {
    if (r.error() == std::errc::permission_denied) {
      // EACCES races: paused_ might be set already (steady-state pause)
      // or not yet (kernel revoked before libseat's disable_seat reached
      // us). Either way, skip the frame; in the still-active-flag case
      // log a warning so an unexpected revoke is still visible.
      if (!paused_.load(std::memory_order_acquire)) {
        ihs::log::warn(
            "[DrmCompositor] framed commit: master revoked (EACCES); skip "
            "frame");
      }
      return false;
    }
    ihs::log::warn(
        "[DrmCompositor] framed atomic commit failed ({}); latching GL "
        "fallback",
        r.error().message());
    ihs::log::error(
        "[DrmCompositor] framed config ({}x{} on {}x{} mode) requires atomic "
        "plane support; GL fallback cannot letterbox. Next Present will fail.",
        fb_w, fb_h, mode_w, mode_h);
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, count);
  }

  SettleAtomicCommit(blocking);
  comp_idx_ ^= 1;

  if (profile && profile_) {
    const uint64_t t4 = NsNow();
    profile_->p.account(t1 - t0, t2 - t1, t3 - t2, t4 - t3, t4 - t0);
    if (profile_->p.frames >= kProfileWindow) {
      const auto& s = profile_->p;
      const auto ms = [&](uint64_t ns_sum) {
        return static_cast<double>(ns_sum) /
               (static_cast<double>(s.frames) * 1e6);
      };
      const auto ms_max = [](uint64_t ns) {
        return static_cast<double>(ns) / 1e6;
      };
      ihs::log::info(
          "[DrmCompositor] framed profile (n={}): "
          "wait={:.2f}ms (max {:.2f})  compose={:.2f}ms (max {:.2f})  "
          "test={:.2f}ms (max {:.2f})  commit={:.2f}ms (max {:.2f})  "
          "total={:.2f}ms (max {:.2f})",
          s.frames, ms(s.wait_sum_ns), ms_max(s.wait_max_ns),
          ms(s.compose_sum_ns), ms_max(s.compose_max_ns), ms(s.test_sum_ns),
          ms_max(s.test_max_ns), ms(s.commit_sum_ns), ms_max(s.commit_max_ns),
          ms(s.total_sum_ns), ms_max(s.total_max_ns));
      profile_->p.reset();
    }
  }
  return true;
}

// ─── Post-modeset pipe verification ──────────────────────────────────────

void DrmCompositor::VerifyPipeRunning() const {
  const int fd = backend_->drm_fd();
  const uint32_t crtc_id = out_.crtc_id();

  // ── CRTC.ACTIVE readback ──
  // Walk CRTC properties looking for "ACTIVE". If we can't find it or
  // it's 0, the kernel didn't actually bring the pipe up despite our
  // ALLOW_MODESET commit returning success.
  uint64_t crtc_active = 0;
  bool crtc_active_found = false;
  if (auto* props =
          drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC)) {
    for (uint32_t i = 0; i < props->count_props; ++i) {
      if (auto* p = drmModeGetProperty(fd, props->props[i])) {
        if (std::string_view(p->name) == "ACTIVE") {
          crtc_active = props->prop_values[i];
          crtc_active_found = true;
        }
        drmModeFreeProperty(p);
      }
    }
    drmModeFreeObjectProperties(props);
  }
  if (!crtc_active_found) {
    ihs::log::warn(
        "[DrmCompositor] pipe-check: CRTC.ACTIVE property not found");
  } else if (crtc_active != 1) {
    ihs::log::error(
        "[DrmCompositor] pipe-check: CRTC {} NOT active (ACTIVE={}) after "
        "modeset commit. The atomic commit reported success but the kernel "
        "did not activate the pipe — no PAGE_FLIP_EVENTs will fire.",
        crtc_id, crtc_active);
  } else {
    ihs::log::info("[DrmCompositor] pipe-check: CRTC {} ACTIVE=1", crtc_id);
  }

  // ── Primary plane FB_ID readback ──
  uint32_t primary_plane_id = 0;
  if (plane_registry_) {
    for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
      if (p->type == drm::planes::DRMPlaneType::PRIMARY) {
        primary_plane_id = p->id;
        break;
      }
    }
  }
  if (primary_plane_id != 0) {
    uint64_t primary_fb = 0;
    uint64_t primary_crtc = 0;
    bool fb_found = false;
    if (auto* props = drmModeObjectGetProperties(fd, primary_plane_id,
                                                 DRM_MODE_OBJECT_PLANE)) {
      for (uint32_t i = 0; i < props->count_props; ++i) {
        if (auto* p = drmModeGetProperty(fd, props->props[i])) {
          if (const std::string_view name(p->name); name == "FB_ID") {
            primary_fb = props->prop_values[i];
            fb_found = true;
          } else if (name == "CRTC_ID") {
            primary_crtc = props->prop_values[i];
          }
          drmModeFreeProperty(p);
        }
      }
      drmModeFreeObjectProperties(props);
    }
    if (!fb_found || primary_fb == 0) {
      ihs::log::error(
          "[DrmCompositor] pipe-check: primary plane {} has no FB_ID "
          "(found={}, value={}). Kernel silently accepted commit without "
          "binding a framebuffer — nothing to scan out.",
          primary_plane_id, fb_found, primary_fb);
    } else {
      ihs::log::info(
          "[DrmCompositor] pipe-check: primary plane {} FB_ID={} CRTC_ID={}",
          primary_plane_id, primary_fb, primary_crtc);
    }
  }

  // ── Vblank sequence probe ──
  // ACTIVE=1 and a bound FB don't guarantee the pipe is *ticking*.
  // drmCrtcGetSequence reports the vblank counter — sample before, sleep
  // ~2 refresh periods, sample after. No advance ⇒ pipe stuck (DPMS off,
  // wrong connector, format mismatch, driver silent-accept bug). This is
  // the definitive "is scanout alive" test.
  uint64_t seq_before = 0;
  uint64_t ns_before = 0;
  if (drmCrtcGetSequence(fd, crtc_id, &seq_before, &ns_before) != 0) {
    ihs::log::warn(
        "[DrmCompositor] pipe-check: drmCrtcGetSequence unsupported or "
        "failed ({}); skipping vblank probe",
        std::strerror(errno));
    return;
  }
  const uint32_t vrefresh = out_.vrefresh() > 0 ? out_.vrefresh() : 60;
  const useconds_t sleep_us = 2'000'000u / vrefresh;  // 2 refresh periods
  ::usleep(sleep_us);
  uint64_t seq_after = 0;
  uint64_t ns_after = 0;
  if (drmCrtcGetSequence(fd, crtc_id, &seq_after, &ns_after) != 0) {
    ihs::log::warn("[DrmCompositor] pipe-check: drmCrtcGetSequence (after): {}",
                   std::strerror(errno));
    return;
  }
  if (seq_after == seq_before) {
    ihs::log::error(
        "[DrmCompositor] pipe-check: vblank sequence did NOT advance over "
        "~{} ms (seq={}). CRTC {} is not scanning out — PAGE_FLIP_EVENTs "
        "will never fire. Likely causes: connector DPMS off, wrong "
        "connector bound, primary format/modifier unsupported for scanout, "
        "or driver silent-accept bug.",
        sleep_us / 1000, seq_before, crtc_id);
  } else {
    ihs::log::info(
        "[DrmCompositor] pipe-check: vblank advanced {} → {} ({} frames in "
        "~{} ms) — pipe is live",
        seq_before, seq_after, seq_after - seq_before, sleep_us / 1000);
  }
}

// ─── PresentLayers (plane-allocator path) ────────────────────────────────

bool DrmCompositor::PresentView(const int64_t /*view_id*/,
                                const FlutterLayer** layers,
                                const size_t layer_count) {
  // Single output today: every view scans out to the one CRTC. When the
  // view_id -> Output map lands, this dispatches to the target output's scene.
  return PresentLayers(layers, layer_count);
}

bool DrmCompositor::PresentLayers(const FlutterLayer** layers,
                                  const size_t layer_count) {
  // Session-pause gate. While inactive, any atomic commit returns
  // EACCES and any flip event never fires. Ack the frame to Flutter
  // (return true) without touching the kernel. The vsync baton stays
  // parked in the provider; the backend's OnSessionResumed drains it to
  // restart the pacer, after which the next Present does a full
  // re-modeset (plane_mode_set_ is cleared in OnResume).
  if (paused_.load(std::memory_order_acquire)) {
    return true;
  }

  // One-shot post-resume probe.
  if (!resume_pending_logged_ && !plane_mode_set_) {
    resume_pending_logged_ = true;
    ihs::log::info(
        "[DrmCompositor] PresentLayers entered post-resume; layer_count={} "
        "framed={} fallback_latched={}",
        layer_count, framed_, fallback_latched_);
  }

  backend_->MaybeCaptureSnapshot();
  EnsureGlCapsProbed();

  // Runtime latch: once an atomic commit fails irrecoverably we stay on
  // the GL path for the rest of the session. Keeps the user in a known-
  // working state without silent retries hammering the kernel.
  if (fallback_latched_ || !planes_available_) {
    return PresentViaGlFallback(layers, layer_count);
  }

  // Framed configs use a dedicated primary-BG + overlay-content path
  // (see PresentFramed). Skips the drm-cxx Allocator's
  // composition-on-primary assumption that amdgpu DC rejects.
  if (framed_) {
    return PresentFramed(layers, layer_count);
  }

#if USE_DRM_SCENE
  // Zero-copy direct-overlay path (primary lacks REFLECT_Y): BG on the
  // primary, the single Flutter BS direct-scanned on a REFLECT_Y
  // overlay. Owns its own GL-fallback decision for multi-layer / PV
  // frames, same as the scene path below.
  if (direct_overlay_mode_) {
    return PresentDirectOverlay(layers, layer_count);
  }

  // Non-framed path under USE_DRM_SCENE: route every frame through
  // LayerScene. PresentLayersViaScene owns the fallback decision —
  // returns true on a successful scene commit, false (after invoking
  // PresentViaGlFallback itself) when any layer would be Composited
  // (BS overflow), when the frame contains a platform view (not yet
  // wired into the scene path), or on commit failure that latches
  // fallback for the rest of the session. Also returns false on a
  // skip-frame condition (EACCES / pause race).
  if (scene_) {
    return PresentLayersViaScene(layers, layer_count);
  }
#endif

  // Profile fence post t0 (entry). Same wait/compose/commit/total
  // breakdown as PresentFramed; logs every kProfileWindow frames when
  // IVI_DRM_PROFILE=1.
  const bool profile = ProfileEnabled();
  const uint64_t t0 = profile ? NsNow() : 0;

  // Wait for any in-flight atomic commit before building the next frame.
  // Baton return is handled inside PageFlipHandler (vsync-locked) and
  // the first-frame path below, so no baton work is needed here.
  if (!WaitForPendingFlip()) {
    return PresentViaGlFallback(layers, layer_count);
  }
  const uint64_t t1 = profile ? NsNow() : 0;

  // ── Build the drm-cxx Output with one Layer per Flutter layer ──

  drm::planes::Output output(out_.crtc_id(), comp_layer_);

  // Letterbox offsets: Flutter layers carry offsets in FB coordinates,
  // but planes land on CRTC coordinates. When the FB is smaller than the
  // mode (framed config), every plane rect shifts by the same centering
  // delta so Flutter still sees a (0,0)-origin surface.
  const uint32_t fb_w = out_.width();
  const uint32_t fb_h = out_.height();
  const uint32_t mode_w = out_.mode_width();
  const uint32_t mode_h = out_.mode_height();
  const int32_t letterbox_x = static_cast<int32_t>(mode_w - fb_w) / 2;
  const int32_t letterbox_y = static_cast<int32_t>(mode_h - fb_h) / 2;

  // Collect backing-store layers and add them as Output layers. Platform
  // views without a scanout-capable BO get folded into the composition
  // buffer (i.e., set_composited()).
  struct FrameLayer {
    const FlutterLayer* flutter;
    GbmBackingStore* store;   // non-null for backing-store layers
    drm::planes::Layer* drm;  // the matching drm-cxx Layer
  };
  std::vector<FrameLayer> frame_layers;
  frame_layers.reserve(layer_count);

  for (size_t i = 0; i < layer_count; ++i) {
    const FlutterLayer* fl = layers[i];
    if (!fl) {
      continue;
    }

    auto& drm_layer = output.add_layer();

    GbmBackingStore* store = nullptr;
    if (fl->type == kFlutterLayerContentTypeBackingStore && fl->backing_store) {
      if (const auto* baton =
              static_cast<StoreBaton*>(fl->backing_store->user_data)) {
        store = baton->store;
      }
    }

    // Direct-scanout requires a KMS framebuffer on the store's BO
    // AND a primary plane that can flip GL-rendered (bottom-up) bits
    // to scanout (top-down) — i.e. REFLECT_Y in its rotation
    // bitmask. Without that the BS would scan out upside-down, so
    // bail to composition where the GL compositor's sample-with-flip
    // already handles the orientation difference.
    if (store && EnsureDrmFbId(*store) && any_plane_supports_reflect_y_) {
      // PixelFormat + FbModifier are *static compatibility hints* the
      // allocator reads BEFORE running TEST_ONLY commits — without
      // them, `Layer::format()` returns nullopt and
      // `Allocator::is_compatible_static_screen` returns false for
      // every plane, so the allocator marks every layer as overflow
      // and we end up GL-compositing everything. See LayerScene's
      // equivalent path (third_party/drm-cxx/src/scene/layer_scene.cpp
      // ~line 942) — same hints, same reason.
      drm_layer.set_property(drm::planes::PropTag::PixelFormat, store->format)
          .set_property(drm::planes::PropTag::FbModifier,
                        gbm_bo_get_modifier(store->active().bo));
      // Flutter renders to the BS using GL's bottom-up NDC convention:
      // pixel rows live in memory with the visual bottom at the low
      // address. The composite path compensates by sampling with
      // flip_y=true into the comp BO before scanout. Direct-scanout
      // has no compose step, so we ask KMS to mirror the plane on the
      // way out (REFLECT_Y). amdgpu DC primary + overlay planes
      // support this; on a driver that doesn't, the allocator's
      // supports_rotation static screen rejects the layer and we fall
      // back to GL composition.
      constexpr uint64_t kReflectY = DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y;
      drm_layer.set_property("FB_ID", store->active().drm_fb_id)
          .set_property("CRTC_ID", out_.crtc_id())
          .set_property("CRTC_X",
                        static_cast<uint64_t>(
                            static_cast<int64_t>(fl->offset.x) + letterbox_x))
          .set_property("CRTC_Y",
                        static_cast<uint64_t>(
                            static_cast<int64_t>(fl->offset.y) + letterbox_y))
          .set_property("CRTC_W", static_cast<uint64_t>(fl->size.width))
          .set_property("CRTC_H", static_cast<uint64_t>(fl->size.height))
          .set_property("SRC_X", 0)
          .set_property("SRC_Y", 0)
          .set_property("SRC_W", static_cast<uint64_t>(store->width) << 16)
          .set_property("SRC_H", static_cast<uint64_t>(store->height) << 16)
          .set_property("rotation", kReflectY);
      // Skip explicit zpos: LayerScene deliberately omits it (line ~945
      // in layer_scene.cpp) because emitting zpos=N where N matches
      // primary's immutable zpos can mis-screen overlays. Letting the
      // allocator pick by plane class works better.
      drm_layer.set_content_type(i == 0 ? drm::planes::ContentType::UI
                                        : drm::planes::ContentType::Generic);
      // Diagnostic (under --debug-backend): track whether the BS BO
      // actually rotates between frames on direct-scanout. With
      // pool_size=3 every frame's bo differs from the last, so
      // unconditional logging would emit ~120 lines/s at 120 Hz.
      // Useful for confirming the pool is doing its job on a new
      // driver / config.
      if (backend_->cfg_.debug_backend) {
        static const gbm_bo* last_bo = nullptr;
        static int distinct_count = 0;
        const auto& s = store->active();
        if (s.bo != last_bo) {
          ++distinct_count;
          ihs::log::debug(
              "[DrmCompositor] direct-scanout bo cycle: bo={} fb_id={} "
              "(distinct={})",
              static_cast<const void*>(s.bo), s.drm_fb_id, distinct_count);
          last_bo = s.bo;
        }
      }
    } else {
      // Platform view or store without a KMS FB — must be composited.
      drm_layer.set_composited();
    }

    frame_layers.push_back({fl, store, &drm_layer});
  }

  // Composition layer on the primary plane. When the FB equals the CRTC
  // mode (no framing), this covers the full display at (0,0). When the
  // user requested a smaller size, the FB is centered on the CRTC and
  // pixels outside scan out as black — no extra FB or per-frame blit.
  // zpos must match the primary plane's legal range — on amdgpu the
  // primary plane has zpos=[2,2] (immutable), so a hardcoded zpos=0
  // here produces EINVAL on commit. primary_zpos_ is the value the
  // allocator already uses for the backing-store layer stack.
  auto& comp = comp_bufs_[comp_idx_];
  comp_layer_.set_property("FB_ID", comp.active().drm_fb_id)
      .set_property("CRTC_ID", out_.crtc_id())
      .set_property("CRTC_X", static_cast<uint64_t>(letterbox_x))
      .set_property("CRTC_Y", static_cast<uint64_t>(letterbox_y))
      .set_property("CRTC_W", fb_w)
      .set_property("CRTC_H", fb_h)
      .set_property("SRC_X", 0)
      .set_property("SRC_Y", 0)
      .set_property("SRC_W", static_cast<uint64_t>(fb_w) << 16)
      .set_property("SRC_H", static_cast<uint64_t>(fb_h) << 16)
      .set_property("zpos", primary_zpos_);

  // ── Run the allocator ──
  // ALLOW_MODESET is only needed on the first atomic commit (the kernel
  // needs to transition from the legacy saved CRTC / no-scanout state
  // into our atomic state). Subsequent commits are pure flips.

  const uint32_t test_flags =
      DRM_MODE_ATOMIC_TEST_ONLY |
      (plane_mode_set_ ? 0u : DRM_MODE_ATOMIC_ALLOW_MODESET);

  drm::AtomicRequest req(backend_->device());

  // First commit only: attach MODE_ID / ACTIVE / Connector.CRTC_ID so
  // the kernel actually activates the pipe. Plane-property writes alone
  // don't bring up the CRTC.
  if (!plane_mode_set_ && modeset_) {
    if (auto r = modeset_->attach(req); !r) {
      ihs::log::warn("[DrmCompositor] modeset attach: {}; falling back to GL",
                     r.error().message());
      return PresentViaGlFallback(layers, layer_count);
    }
  }

  auto result = allocator_->apply(output, req, test_flags);
  if (!result) {
    if (result.error() == std::errc::permission_denied) {
      // libseat pause race: kernel revoked master before pause_cb
      // toggled paused_. Skip the frame either way; warn only when
      // paused_ isn't set so an unexpected revoke remains visible.
      if (!paused_.load(std::memory_order_acquire)) {
        ihs::log::warn(
            "[DrmCompositor] allocator: master revoked (EACCES); skip frame");
      }
      return false;
    }
    ihs::log::warn("[DrmCompositor] allocator: {}; falling back to GL",
                   result.error().message());
    return PresentViaGlFallback(layers, layer_count);
  }
  if (backend_->cfg_.debug_backend) {
    ihs::log::info("[DrmCompositor] {} of {} layers on HW planes", *result,
                   frame_layers.size());
    for (size_t i = 0; i < frame_layers.size(); ++i) {
      const auto& fl = frame_layers[i];
      if (auto pid = fl.drm->assigned_plane_id()) {
        ihs::log::info(
            "[DrmCompositor]   layer {} → plane {} (fb_id={}, {}x{}, zpos={})",
            i, *pid, fl.store ? fl.store->active().drm_fb_id : 0,
            fl.store ? fl.store->width : 0, fl.store ? fl.store->height : 0,
            primary_zpos_ + i);
      } else if (fl.drm->needs_composition()) {
        ihs::log::info("[DrmCompositor]   layer {} → composition (overflow)",
                       i);
      } else {
        ihs::log::info("[DrmCompositor]   layer {} → unassigned?!", i);
      }
    }
    const auto comp_pid = comp_layer_.assigned_plane_id();
    ihs::log::info(
        "[DrmCompositor]   comp_layer → {} (fb_id={})",
        comp_pid ? std::to_string(*comp_pid) : std::string("<unassigned>"),
        comp.active().drm_fb_id);
  }

  // ── GL-composite layers that overflowed into the composition buffer ──
  //
  // Direct-scanout fast path: when the allocator placed every Flutter
  // layer on its own plane, there's nothing to composite — skip the
  // entire GL block (bind / clear / glFinish) and let the comp buffer
  // retain its prior content. It won't be scanned out: comp_layer is
  // either unassigned by the allocator, or assigned to primary and
  // covered by the per-layer scanout overlays above it. On a fullscreen
  // single-BS frame this cuts the per-Present GL cost from ~6ms to
  // ~0ms in local 2560x1440@240Hz measurements.
  bool needs_compositing = false;
  for (const auto& fl : frame_layers) {
    if (fl.drm->needs_composition()) {
      needs_compositing = true;
      break;
    }
  }

  bool any_composited = false;
  if (needs_compositing) {
    glBindFramebuffer(GL_FRAMEBUFFER, comp.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    for (auto& [flutter, store, drm] : frame_layers) {
      if (!drm->needs_composition()) {
        continue;
      }
      const bool blend = any_composited;
      if (store) {
        CompositeLayerIntoFbo(comp.fbo, store->fbo, store->active().color_tex,
                              static_cast<GLsizei>(store->width),
                              static_cast<GLsizei>(store->height),
                              static_cast<GLint>(flutter->offset.x),
                              static_cast<GLint>(flutter->offset.y),
                              static_cast<GLsizei>(flutter->size.width),
                              static_cast<GLsizei>(flutter->size.height), blend,
                              /*flip_y=*/true);
        any_composited = true;
      } else if (flutter->type == kFlutterLayerContentTypePlatformView &&
                 flutter->platform_view) {
        std::shared_ptr<ICompositorSurface> surface_sp;
        {
          std::lock_guard<std::mutex> lock(surfaces_mu_);
          auto it = surfaces_.find(flutter->platform_view->identifier);
          if (it != surfaces_.end()) {
            surface_sp = it->second;
          }
        }
        if (surface_sp) {
          surface_sp->OnPresent(flutter);
          if (const auto tex = surface_sp->GetGlTextureName(); tex != 0) {
            // See comp.fbo rationale in PresentFramed's platform-view
            // branch.
            const bool flip_y = !surface_sp->TextureIsTopFirst();
            CompositeLayerIntoFbo(
                comp.fbo, /*src_fbo=*/0, tex, surface_sp->GetGlTextureWidth(),
                surface_sp->GetGlTextureHeight(),
                static_cast<GLint>(flutter->offset.x),
                static_cast<GLint>(flutter->offset.y),
                static_cast<GLsizei>(flutter->size.width),
                static_cast<GLsizei>(flutter->size.height), blend, flip_y);
            any_composited = true;
          }
        }
      }
    }

    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  // Explicit GPU↔display sync for the direct-scanout path. The GL
  // composite block above ends in glFinish (covers both Flutter's BS
  // writes and our composite), but direct-scanout skips composite
  // entirely. On drivers that don't insert implicit dma-fence on
  // commit (notably nvidia-drm), the kernel scans before Flutter's
  // writes have landed → visible artifacts. Hand the kernel an
  // IN_FENCE_FD that signals when Flutter's GPU work is done; the
  // kernel waits asynchronously rather than us stalling the
  // rasterizer thread.
  //
  // Fallback: if the EGL native-fence-sync extension isn't available,
  // glFinish here is the only option — same correctness, ~2 ms
  // synchronous stall.
  int in_fence_fd = -1;
  // RAII: close the dup'd fence FD on every exit path from this
  // function. The kernel takes its own reference during atomic_check,
  // so closing after the commit returns (success or failure) is safe.
  struct FenceFdCloser {
    int& fd;
    ~FenceFdCloser() {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  } fence_closer{in_fence_fd};

  if (!needs_compositing) {
    if (has_native_fence_sync_) {
      glFlush();  // make sure pending GL work is queued for the fence
      EGLSyncKHR sync = eglCreateSyncKHR_(
          backend_->egl_display(), EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
      if (sync != EGL_NO_SYNC_KHR) {
        in_fence_fd =
            eglDupNativeFenceFDANDROID_(backend_->egl_display(), sync);
        eglDestroySyncKHR_(backend_->egl_display(), sync);
        if (in_fence_fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
          in_fence_fd = -1;
        }
      }
    }
    if (in_fence_fd < 0) {
      // Extension missing, sync creation failed, or fence FD couldn't
      // be dup'd — fall back to synchronous GPU drain.
      glFinish();
    } else {
      // Attach the fence FD to every direct-scanout layer's plane.
      for (auto& [flutter, store, drm] : frame_layers) {
        if (store && any_plane_supports_reflect_y_ &&
            store->active().drm_fb_id != 0) {
          drm->set_property("IN_FENCE_FD", static_cast<uint64_t>(in_fence_fd));
        }
      }
    }
  }

  // ── Atomic commit ──
  // The test-only apply() above already populated `req` with all plane
  // property assignments. Commit it with the real flags — no need to
  // re-apply.

  if (backend_->cfg_.debug_backend) {
    backend_->flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }

  // Stage the HW cursor into this commit (rides the same flip; see
  // StageCursorInto). Its first plane-enable forces a blocking frame.
  //
  // Two-phase flags:
  //   First frame: blocking + ALLOW_MODESET, no PAGE_FLIP_EVENT. The
  //     kernel won't deliver a flip-event on a modeset transition on
  //     most drivers — the commit itself returning is our signal.
  //   Steady state: NONBLOCK + PAGE_FLIP_EVENT for vsync-locked flips.
  //     ALLOW_MODESET is not combined with NONBLOCK unless the driver
  //     has opted in via --drm-allow-nonblock-modeset.
  const bool was_first_commit_pre = !plane_mode_set_;
  // `blocking` also covers a cursor first-enable; `was_first_commit_pre`
  // stays "initial modeset" for the slot-pipeline bookkeeping below.
  const bool blocking = was_first_commit_pre || StageCursorInto(req);
  const uint32_t commit_flags =
      blocking ? DRM_MODE_ATOMIC_ALLOW_MODESET
               : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);

  // Profile fence post: end of allocator + compose + req build, just
  // before the kernel-side commit.
  const uint64_t t2 = profile ? NsNow() : 0;

  if (auto commit_ok = req.commit(commit_flags, static_cast<IFlipSink*>(this));
      !commit_ok) {
    if (commit_ok.error() == std::errc::permission_denied) {
      // EACCES race with libseat pause; warn only when paused_ hasn't
      // toggled yet so unexpected revokes still surface.
      if (!paused_.load(std::memory_order_acquire)) {
        ihs::log::warn(
            "[DrmCompositor] atomic commit: master revoked (EACCES); skip "
            "frame");
      }
      return false;
    }
    // Latch: stop trying planes for the rest of the session. One WARN
    // per latch so the failure is visible without flooding.
    ihs::log::warn(
        "[DrmCompositor] atomic commit failed ({}); latching GL fallback "
        "for remaining session",
        commit_ok.error().message());
    if (out_.width() != out_.mode_width() ||
        out_.height() != out_.mode_height()) {
      ihs::log::error(
          "[DrmCompositor] framed config ({}x{} on {}x{} mode) needs the "
          "atomic plane compositor; legacy fallback can't letterbox. Next "
          "Present will fail.",
          out_.width(), out_.height(), out_.mode_width(), out_.mode_height());
    }
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, layer_count);
  }

  SettleAtomicCommit(blocking);
  // Only advance the comp-buffer double-buffer index if we actually
  // wrote to it this frame. Direct-scanout frames don't touch comp,
  // so the next frame can keep using the same comp_idx_ if it needs
  // composition again.
  if (any_composited) {
    comp_idx_ ^= 1;
  }
  output.mark_clean();

  // Rotate each Flutter BS pool that participated in this frame. The
  // commit handed the kernel each store's active slot's FB_ID; Flutter's
  // next render must target a *different* slot so we don't overwrite
  // content the kernel is still scanning out — the visible-flicker mode
  // on drivers that don't kernel-side hold the BO during scanout
  // (notably nvidia-drm).
  //
  // Round-robin with pool_size >= 3 is empirically safe: by the time
  // we cycle back to a slot it's been ~2 vblanks since the kernel last
  // referenced it. The slot-release pipeline (in_flight_slots_ +
  // scanning_slots_) tracks the actual state so a later size of 2
  // would also be correct.
  //
  // Stores with pool_size == 1 (comp_bufs_, bg_store_) skip the rotate;
  // they're already multi-buffered at a higher level (comp_idx_) or
  // single-shot (bg_store_).
  bool any_rotated = false;
  std::vector<SlotRef> committed_this_frame;
  for (const auto& fl : frame_layers) {
    if (!fl.store || fl.store->pool_size <= 1) {
      continue;
    }
    auto& store = *fl.store;
    const size_t committed_idx = store.active_idx;
    store.pool[committed_idx].state = GbmBackingStore::Slot::State::Pending;
    committed_this_frame.push_back({&store, committed_idx});
    store.active_idx = (store.active_idx + 1) % store.pool_size;
    store.pool[store.active_idx].state = GbmBackingStore::Slot::State::Active;
    // Rebind the FBO's color attachment to the new active slot's
    // texture. The FBO ID we returned to Flutter via
    // FlutterBackingStore::open_gl.framebuffer.name is unchanged; only
    // the attachment is swapped, so Flutter's cached FBO state stays
    // valid.
    glBindFramebuffer(GL_FRAMEBUFFER, store.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           store.active().color_tex, 0);
    any_rotated = true;
    if (backend_->cfg_.debug_backend) {
      GLint attached_tex = 0;
      glGetFramebufferAttachmentParameteriv(
          GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attached_tex);
      if (static_cast<GLuint>(attached_tex) != store.active().color_tex) {
        ihs::log::warn(
            "[DrmCompositor] FBO rebind verify failed: expected "
            "color_tex={} got={}",
            store.active().color_tex, attached_tex);
      }
    }
  }
  // Only restore the default FBO when we actually rebound. Calling
  // glBindFramebuffer(0) unconditionally per frame triggered GL-driver
  // state-flush jitter under image-decode-heavy workloads on Tegra
  // (sustained 60 Hz lock degraded to 25 fps after ~9 s); gating on
  // any_rotated keeps the call out of the hot path when no Flutter BS
  // participated in this frame.
  if (any_rotated) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  // Hand off committed slots to the release pipeline. The first commit
  // was blocking + ALLOW_MODESET, so no PAGE_FLIP_EVENT will fire —
  // promote the slots straight to "scanning" so the next event frees
  // them. Subsequent commits queue and the flip handler drains.
  if (!committed_this_frame.empty()) {
    std::lock_guard<std::mutex> lock(slot_pipeline_mu_);
    if (was_first_commit_pre) {
      for (auto& ref : scanning_slots_) {
        ref.store->pool[ref.slot_idx].state =
            GbmBackingStore::Slot::State::Free;
      }
      scanning_slots_ = std::move(committed_this_frame);
    } else {
      in_flight_slots_.push_back(std::move(committed_this_frame));
    }
  }

  if (profile && profile_) {
    const uint64_t t3 = NsNow();
    const uint64_t wait_ns = t1 - t0;
    const uint64_t compose_ns = t2 - t1;
    const uint64_t commit_ns = t3 - t2;
    const uint64_t total_ns = t3 - t0;
    // PresentLayers doesn't run a TEST_ONLY probe in steady state; t3
    // is end-of-commit, "test" stays 0 in the summary.
    profile_->p.account(wait_ns, compose_ns, /*test=*/0, commit_ns, total_ns);
    // Per-frame slow-frame log: any frame whose total exceeds 1.5 ×
    // the vblank period missed its target (or got close enough that
    // the next miss is one cosmic-ray away). Print the breakdown so
    // it's obvious which stage spiked — wait/compose/commit. Cheap
    // because slow frames are rare by definition.
    const uint64_t period_ns =
        out_.vrefresh() > 0 ? 1000000000ULL / out_.vrefresh() : 16666667ULL;
    const uint64_t slow_threshold_ns = (period_ns * 3) / 2;
    if (total_ns > slow_threshold_ns) {
      ihs::log::info(
          "[DrmCompositor] slow frame: wait={:.2f}ms compose={:.2f}ms "
          "commit={:.2f}ms total={:.2f}ms (vblank budget {:.2f}ms)",
          static_cast<double>(wait_ns) / 1e6,
          static_cast<double>(compose_ns) / 1e6,
          static_cast<double>(commit_ns) / 1e6,
          static_cast<double>(total_ns) / 1e6,
          static_cast<double>(period_ns) / 1e6);
    }
    if (profile_->p.frames >= kProfileWindow) {
      const auto& s = profile_->p;
      const auto ms = [&](uint64_t ns_sum) {
        return static_cast<double>(ns_sum) /
               (static_cast<double>(s.frames) * 1e6);
      };
      const auto ms_max = [](uint64_t ns) {
        return static_cast<double>(ns) / 1e6;
      };
      ihs::log::info(
          "[DrmCompositor] planes profile (n={}): "
          "wait={:.2f}ms (max {:.2f})  compose={:.2f}ms (max {:.2f})  "
          "commit={:.2f}ms (max {:.2f})  total={:.2f}ms (max {:.2f})",
          s.frames, ms(s.wait_sum_ns), ms_max(s.wait_max_ns),
          ms(s.compose_sum_ns), ms_max(s.compose_max_ns), ms(s.commit_sum_ns),
          ms_max(s.commit_max_ns), ms(s.total_sum_ns), ms_max(s.total_max_ns));
      profile_->p.reset();
    }
  }
  return true;
}

#if USE_DRM_SCENE
// ─── LayerScene-driven non-framed present path ───────────────────────────

bool DrmCompositor::PresentLayersViaScene(const FlutterLayer** layers,
                                          const size_t layer_count) {
  // Pause / fallback-latched / framed gates have already fired in
  // PresentLayers before dispatching here. Profile fences mirror the
  // legacy non-framed path so IVI_DRM_PROFILE output stays comparable.
  const bool profile = ProfileEnabled();
  const uint64_t t0 = profile ? NsNow() : 0;

  if (!WaitForPendingFlip()) {
    return PresentViaGlFallback(layers, layer_count);
  }
  const uint64_t t1 = profile ? NsNow() : 0;

  // Pre-scan: any platform-view layer forces the whole frame through
  // the GL composite path. Platform-view→LayerBufferSource plumbing
  // (Tier-2 wrappers over ExternalDmaBufSource / GstAppsinkSource) is
  // not wired in this revision; routing PVs through GL keeps the
  // current visual behaviour intact while BS layers benefit from the
  // scene allocator.
  for (size_t i = 0; i < layer_count; ++i) {
    const FlutterLayer* fl = layers[i];
    if (fl && fl->type == kFlutterLayerContentTypePlatformView) {
      return PresentViaGlFallback(layers, layer_count);
    }
  }

  // Walk the FlutterLayer[] in z-order (Flutter's convention: layer 0
  // is bottom). Sync scene_'s layer set against the current frame's
  // batons: add new, update dst_rect on existing, remove any scene
  // layer whose baton didn't appear this frame.
  struct FrameLayer {
    const FlutterLayer* flutter;
    GbmBackingStore* store;
    StoreBaton* baton;
  };
  std::vector<FrameLayer> frame_layers;
  frame_layers.reserve(layer_count);

  // Track this frame's batons for the stale-layer prune below. Linear
  // scan; engaged layer count is single-digit in practice.
  std::vector<StoreBaton*> frame_batons;
  frame_batons.reserve(layer_count);

  for (size_t i = 0; i < layer_count; ++i) {
    const FlutterLayer* fl = layers[i];
    if (!fl || fl->type != kFlutterLayerContentTypeBackingStore ||
        !fl->backing_store) {
      continue;
    }
    auto* baton = static_cast<StoreBaton*>(fl->backing_store->user_data);
    if (!baton || !baton->store) {
      continue;
    }
    frame_layers.push_back({fl, baton->store, baton});
    frame_batons.push_back(baton);
  }

  // Prune scene layers whose batons are not in this frame's set. The
  // scene retains layers across commits; without this step, a layer
  // for a backing store Flutter dropped from its layer tree (without
  // collecting it yet) would still be acquired by the next commit
  // with stale dst_rect.
  for (auto it = scene_layer_batons_.begin();
       it != scene_layer_batons_.end();) {
    StoreBaton* baton = *it;
    const bool still_present =
        std::find(frame_batons.begin(), frame_batons.end(), baton) !=
        frame_batons.end();
    if (!still_present) {
      if (auto* layer = scene_->find_by_identity_tag(baton)) {
        scene_->remove_layer(layer->handle());
      }
      it = scene_layer_batons_.erase(it);
    } else {
      ++it;
    }
  }

  // Add new layers; update dst_rect on existing ones. dst_rect is in
  // CRTC coordinates; the non-framed path has no letterbox so
  // FlutterLayer.offset is already CRTC-relative.
  for (auto& fl : frame_layers) {
    drm::scene::Rect dst_rect{
        static_cast<int32_t>(fl.flutter->offset.x),
        static_cast<int32_t>(fl.flutter->offset.y),
        static_cast<uint32_t>(fl.flutter->size.width),
        static_cast<uint32_t>(fl.flutter->size.height),
    };
    auto* layer = scene_->find_by_identity_tag(fl.baton);
    if (layer) {
      layer->set_dst_rect_if_changed(dst_rect);
      continue;
    }
    // First sight: hand the scene_source wrapper to the scene.
    // CreateBackingStore pre-built it; nullptr would only happen if
    // the store predated the scene_ construction, which can't happen
    // on the non-framed path (scene_ is built before any Present).
    if (!fl.baton->scene_source) {
      ihs::log::warn(
          "[DrmCompositor] LayerScene: baton lacks scene_source; routing "
          "frame through GL fallback");
      return PresentViaGlFallback(layers, layer_count);
    }
    drm::scene::LayerDesc desc{};
    desc.source = std::move(fl.baton->scene_source);
    desc.display.dst_rect = dst_rect;
    desc.display.rotation = DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y;
    desc.content_type = (&fl == &frame_layers.front())
                            ? drm::planes::ContentType::UI
                            : drm::planes::ContentType::Generic;
    desc.identity_tag = fl.baton;
    auto handle = scene_->add_layer(std::move(desc));
    if (!handle) {
      ihs::log::warn("[DrmCompositor] LayerScene::add_layer: {}",
                     handle.error().message());
      return PresentViaGlFallback(layers, layer_count);
    }
    scene_layer_batons_.push_back(fl.baton);
  }

  // TEST_ONLY probe: if any layer would land in the composition
  // bucket, route the frame through PresentViaGlFallback. The GL
  // composite path is materially faster than drm-cxx's CompositeCanvas
  // over uncached GBM read-back for backing-store layers, and PVs
  // already short-circuited above.
  auto test = scene_->test();
  if (!test) {
    if (test.error() == std::errc::permission_denied) {
      if (!paused_.load(std::memory_order_acquire)) {
        ihs::log::warn(
            "[DrmCompositor] LayerScene::test: master revoked (EACCES); "
            "skip frame");
      }
      return false;
    }
    ihs::log::warn("[DrmCompositor] LayerScene::test: {}; routing through GL",
                   test.error().message());
    return PresentViaGlFallback(layers, layer_count);
  }
  for (const auto& p : test->placements) {
    if (p.placement == drm::scene::LayerPlacement::Composited ||
        p.placement == drm::scene::LayerPlacement::Unassigned) {
      if (backend_->cfg_.debug_backend) {
        ihs::log::debug(
            "[DrmCompositor] LayerScene test: layer {} {}; routing through "
            "GL fallback",
            p.handle.id,
            p.placement == drm::scene::LayerPlacement::Composited
                ? "composited"
                : "unassigned");
      }
      return PresentViaGlFallback(layers, layer_count);
    }
  }

  // All BS layers placed on planes. Sync GPU writes before scanout —
  // the legacy direct-scanout path uses IN_FENCE_FD when the EGL
  // native-fence-sync extension is present; the scene path falls back
  // to a synchronous drain for now. Correctness-equivalent at the
  // cost of one per-frame stall on Tegra; LayerBufferSource sync
  // hooks are a follow-on.
  glFinish();

  if (backend_->cfg_.debug_backend) {
    backend_->flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }

  // LayerScene injects ALLOW_MODESET implicitly on its first commit,
  // so the embedder doesn't manage plane_mode_set_ for this path.
  const bool was_first_commit_pre = !plane_mode_set_;

  // Two-phase flags, mirroring the legacy direct-scanout path: the
  // first commit is a blocking ALLOW_MODESET with NO PAGE_FLIP_EVENT.
  // The kernel doesn't deliver a flip event across a modeset on most
  // drivers (the commit returning is our signal), and combining the
  // ALLOW_MODESET that LayerScene ORs in for first_commit_ with
  // NONBLOCK makes rockchip's atomic path return EBUSY against the
  // modeset's still-pending flip — wedging the CRTC. Steady state:
  // NONBLOCK + PAGE_FLIP_EVENT for vsync-locked flips.
  const uint32_t commit_flags =
      was_first_commit_pre
          ? DRM_MODE_ATOMIC_ALLOW_MODESET
          : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);
  const uint64_t t2 = profile ? NsNow() : 0;

  auto report = scene_->commit(commit_flags, static_cast<IFlipSink*>(this));
  if (!report) {
    if (report.error() == std::errc::permission_denied) {
      if (!paused_.load(std::memory_order_acquire)) {
        ihs::log::warn(
            "[DrmCompositor] LayerScene::commit: master revoked (EACCES); "
            "skip frame");
      }
      return false;
    }
    ihs::log::warn(
        "[DrmCompositor] LayerScene::commit failed ({}); latching GL "
        "fallback for remaining session",
        report.error().message());
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, layer_count);
  }

  if (was_first_commit_pre) {
    // First commit landed; LayerScene's implicit ALLOW_MODESET cycle
    // is blocking, so no PAGE_FLIP_EVENT is in flight. Latch + verify
    // + kick the initial vsync baton, same as the legacy path.
    plane_mode_set_ = true;
    flip_pending_.store(false, std::memory_order_release);
    VerifyPipeRunning();
    (void)backend_->vsync_.DeliverParkedBaton();
  } else {
    flip_pending_.store(true, std::memory_order_release);
  }

  // LayerScene owns its commit and can't accept a staged cursor plane, so
  // (in staged mode) self-commit the cursor here to keep it visible. The
  // scene already reserves the cursor's plane (set_external_reserved_planes
  // via ApplyCursorReservation). TODO: a LayerScene API that takes the
  // cursor plane in its own commit would avoid this separate commit (which
  // reintroduces the flip contention on nvidia-drm in fullscreen scene
  // mode). No-op when no cursor is staged.
  if (cursor_ != nullptr) {
    cursor_->CommitPending();
  }

  // Rotate every BS pool that participated. Same slot-pipeline
  // bookkeeping as the legacy path — the scene path does not touch
  // these data structures, so the existing in_flight_slots_ /
  // scanning_slots_ release semantics still apply.
  bool any_rotated = false;
  std::vector<SlotRef> committed_this_frame;
  for (const auto& fl : frame_layers) {
    if (!fl.store || fl.store->pool_size <= 1) {
      continue;
    }
    auto& store = *fl.store;
    const size_t committed_idx = store.active_idx;
    store.pool[committed_idx].state = GbmBackingStore::Slot::State::Pending;
    committed_this_frame.push_back({&store, committed_idx});
    store.active_idx = (store.active_idx + 1) % store.pool_size;
    store.pool[store.active_idx].state = GbmBackingStore::Slot::State::Active;
    glBindFramebuffer(GL_FRAMEBUFFER, store.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           store.active().color_tex, 0);
    any_rotated = true;
  }
  if (any_rotated) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  if (!committed_this_frame.empty()) {
    std::lock_guard<std::mutex> lock(slot_pipeline_mu_);
    if (was_first_commit_pre) {
      for (auto& ref : scanning_slots_) {
        ref.store->pool[ref.slot_idx].state =
            GbmBackingStore::Slot::State::Free;
      }
      scanning_slots_ = std::move(committed_this_frame);
    } else {
      in_flight_slots_.push_back(std::move(committed_this_frame));
    }
  }

  if (backend_->cfg_.debug_backend) {
    ihs::log::debug(
        "[DrmCompositor] LayerScene commit: total={} assigned={} "
        "composited={} unassigned={} skipped={} props_written={} "
        "fbs_attached={}",
        report->layers_total, report->layers_assigned,
        report->layers_composited, report->layers_unassigned,
        report->layers_skipped_no_frame, report->properties_written,
        report->fbs_attached);
  }

  if (profile && profile_) {
    const uint64_t t3 = NsNow();
    const uint64_t wait_ns = t1 - t0;
    const uint64_t compose_ns = t2 - t1;
    const uint64_t commit_ns = t3 - t2;
    const uint64_t total_ns = t3 - t0;
    profile_->p.account(wait_ns, compose_ns, /*test=*/0, commit_ns, total_ns);
    if (profile_->p.frames >= kProfileWindow) {
      const auto& s = profile_->p;
      const auto ms = [&](uint64_t ns_sum) {
        return static_cast<double>(ns_sum) /
               (static_cast<double>(s.frames) * 1e6);
      };
      const auto ms_max = [](uint64_t ns) {
        return static_cast<double>(ns) / 1e6;
      };
      ihs::log::info(
          "[DrmCompositor] scene profile (n={}): wait={:.2f}ms (max {:.2f})  "
          "compose={:.2f}ms (max {:.2f})  commit={:.2f}ms (max {:.2f})  "
          "total={:.2f}ms (max {:.2f})",
          s.frames, ms(s.wait_sum_ns), ms_max(s.wait_max_ns),
          ms(s.compose_sum_ns), ms_max(s.compose_max_ns), ms(s.commit_sum_ns),
          ms_max(s.commit_max_ns), ms(s.total_sum_ns), ms_max(s.total_max_ns));
      profile_->p.reset();
    }
  }
  return true;
}

// ─── Direct-overlay mode (zero-copy scene present) ───────────────────────

bool DrmCompositor::ProbePrimaryReflectY(const uint32_t primary_id,
                                         const bool enum_hint) {
  if (primary_id == 0 || !modeset_) {
    return enum_hint;
  }

  // Cache the primary + other non-cursor planes' property IDs locally;
  // the probe disables the others so residual fbcon state can't taint
  // the TEST result.
  const int fd = backend_->drm_fd();
  drm::PropertyStore pp;
  if (auto r = pp.cache_properties(fd, primary_id, DRM_MODE_OBJECT_PLANE); !r) {
    ihs::log::warn("[DrmCompositor] REFLECT_Y probe: cache primary props: {}",
                   r.error().message());
    return enum_hint;
  }
  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR || p->id == primary_id) {
      continue;
    }
    (void)pp.cache_properties(fd, p->id, DRM_MODE_OBJECT_PLANE);
  }

  // The probe needs a real FB on the primary, and it must be the SAME
  // format the direct path actually scans out: the backing-store format
  // (bs_format, an alpha format), NOT primary_format (opaque). vc4's
  // primary accepts XRGB+REFLECT_Y but rejects ARGB+REFLECT_Y, so a BG
  // (XRGB) probe false-positives. Use a throwaway bs_format FB —
  // TEST_ONLY validates config only (format/modifier/rotation/rects), so
  // its pixels need no clear.
  const uint32_t probe_format = out_.resolved().bs_format;
  GbmBackingStore probe_fb{};
  if (!CreateGbmStore(probe_fb, out_.mode_width(), out_.mode_height(),
                      probe_format, /*pool_size=*/1,
                      /*force_scanout=*/true) ||
      !EnsureDrmFbId(probe_fb)) {
    ihs::log::warn("[DrmCompositor] REFLECT_Y probe: test FB create failed");
    DestroyGbmStore(probe_fb);
    return enum_hint;
  }

  const uint32_t crtc_id = out_.crtc_id();
  const uint32_t mode_w = out_.mode_width();
  const uint32_t mode_h = out_.mode_height();

  // TEST_ONLY commit: power up the pipe (modeset), bind the probe FB to
  // the primary at the requested rotation, disable every other non-cursor
  // plane. Returns true iff the kernel validates the transaction. The
  // primary's zpos is deliberately NOT written — it's a fixed slot on
  // these drivers and writing it would EINVAL, masking the REFLECT_Y
  // result we're actually probing for.
  auto try_rotation = [&](const uint64_t rotation) -> bool {
    drm::AtomicRequest req(backend_->device());
    if (!req.valid()) {
      return false;
    }
    if (auto r = modeset_->attach(req); !r) {
      return false;
    }
    auto set = [&](const uint32_t obj, const char* name,
                   const uint64_t value) -> bool {
      auto pid = pp.property_id(obj, name);
      return pid && req.add_property(obj, *pid, value).has_value();
    };
    if (!set(primary_id, "FB_ID", probe_fb.active().drm_fb_id) ||
        !set(primary_id, "CRTC_ID", crtc_id) || !set(primary_id, "CRTC_X", 0) ||
        !set(primary_id, "CRTC_Y", 0) || !set(primary_id, "CRTC_W", mode_w) ||
        !set(primary_id, "CRTC_H", mode_h) || !set(primary_id, "SRC_X", 0) ||
        !set(primary_id, "SRC_Y", 0) ||
        !set(primary_id, "SRC_W", static_cast<uint64_t>(mode_w) << 16) ||
        !set(primary_id, "SRC_H", static_cast<uint64_t>(mode_h) << 16) ||
        !set(primary_id, "rotation", rotation)) {
      return false;
    }
    for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR || p->id == primary_id) {
        continue;
      }
      if (auto fb_pid = pp.property_id(p->id, "FB_ID")) {
        (void)req.add_property(p->id, *fb_pid, 0);
      }
      if (auto crtc_pid = pp.property_id(p->id, "CRTC_ID")) {
        (void)req.add_property(p->id, *crtc_pid, 0);
      }
    }
    constexpr uint32_t test_flags =
        DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    return req.test(test_flags).has_value();
  };

  // Isolate REFLECT_Y: if the primary takes the BS format with REFLECT_Y
  // it's genuinely capable; if it rejects REFLECT_Y but accepts ROTATE_0
  // with the same FB, REFLECT_Y is the specific culprit → route to an
  // overlay. If both fail (primary can't scan this format at all) the
  // probe can't isolate REFLECT_Y, so defer to the enum hint.
  bool result = enum_hint;
  const char* verdict = "inconclusive (both ROTATE_0 and REFLECT_Y failed)";
  if (try_rotation(DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y)) {
    result = true;
    verdict = "accepts REFLECT_Y (direct scanout on primary OK)";
  } else if (try_rotation(DRM_MODE_ROTATE_0)) {
    result = false;
    verdict = "rejects REFLECT_Y, accepts ROTATE_0 -> route BS to overlay";
  }
  DestroyGbmStore(probe_fb);

  ihs::log::info("[DrmCompositor] REFLECT_Y probe: primary {} {} (verdict={})",
                 primary_id, verdict, result);
  return result;
}

bool DrmCompositor::InitDirectOverlay() {
  // Pick the PRIMARY (the opaque-BG target) and the first OVERLAY that
  // advertises both the backing-store scanout format and REFLECT_Y. The
  // overlay carries the bottom-up GL-rendered BS and flips it to scanout
  // order; the primary scans out a mode-sized opaque BG so amdgpu DC's
  // "primary must cover the CRTC" check is satisfied. Mirrors
  // InitFramedMode's plane selection, but the overlay format is the BS
  // (alpha) format and the overlay must also support REFLECT_Y.
  const uint32_t bg_format = out_.resolved().primary_format;
  const uint32_t bs_format = out_.resolved().bs_format;
  const uint64_t want_overlay_zpos = primary_zpos_ + 1;
  const int fd = backend_->drm_fd();

  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::PRIMARY &&
        framed_primary_id_ == 0) {
      framed_primary_id_ = p->id;
      continue;
    }
    if (p->type == drm::planes::DRMPlaneType::OVERLAY &&
        direct_overlay_id_ == 0) {
      if (!p->supports_format(bs_format) ||
          !PlaneSupportsReflectY(fd, static_cast<uint32_t>(p->id))) {
        continue;
      }
      // Clamp the requested zpos to the advertised range; when immutable
      // we take whatever the overlay reports (the commit still succeeds
      // as long as primary and overlay don't collide on one zpos).
      uint64_t zpos = want_overlay_zpos;
      if (p->zpos_min.has_value() && zpos < *p->zpos_min) {
        zpos = *p->zpos_min;
      }
      if (p->zpos_max.has_value() && zpos > *p->zpos_max) {
        zpos = *p->zpos_max;
      }
      direct_overlay_id_ = p->id;
      direct_overlay_zpos_ = zpos;
    }
  }
  if (framed_primary_id_ == 0 || direct_overlay_id_ == 0) {
    ihs::log::info(
        "[DrmCompositor] direct-overlay needs 1 primary + 1 REFLECT_Y overlay "
        "supporting the BS format on CRTC {} (primary={}, overlay={}); using "
        "scene path",
        out_.crtc_id(), framed_primary_id_, direct_overlay_id_);
    return false;
  }

  // Cache property IDs for primary + overlay so PresentDirectOverlay
  // builds the atomic request without per-frame kernel round-trips.
  if (auto r = framed_props_.cache_properties(fd, framed_primary_id_,
                                              DRM_MODE_OBJECT_PLANE);
      !r) {
    ihs::log::error("[DrmCompositor] direct-overlay cache primary props: {}",
                    r.error().message());
    return false;
  }
  if (auto r = framed_props_.cache_properties(fd, direct_overlay_id_,
                                              DRM_MODE_OBJECT_PLANE);
      !r) {
    ihs::log::error("[DrmCompositor] direct-overlay cache overlay props: {}",
                    r.error().message());
    return false;
  }
  // Cache the other non-cursor planes we disable each frame so their
  // FB_ID/CRTC_ID IDs resolve from cache.
  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    if (p->id == framed_primary_id_ || p->id == direct_overlay_id_) {
      continue;
    }
    (void)framed_props_.cache_properties(fd, p->id, DRM_MODE_OBJECT_PLANE);
  }

  // Mode-sized opaque BG pinned to the primary for the session. Filled
  // with black once; the overlay carries all moving pixels. The GL
  // context is current here (InitPlaneAllocator runs inside
  // EnsureGlCapsProbed), so the clear is safe. ProbePrimaryReflectY
  // creates this same store as its TEST FB, so reuse it when valid.
  if (!bg_store_valid_) {
    if (!CreateGbmStore(bg_store_, out_.mode_width(), out_.mode_height(),
                        bg_format, /*pool_size=*/1,
                        /*force_scanout=*/true) ||
        !EnsureDrmFbId(bg_store_)) {
      ihs::log::error(
          "[DrmCompositor] direct-overlay BG GBM store create failed");
      return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, bg_store_.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();  // commit BG pixels before the first atomic scanout binds it
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bg_store_valid_ = true;
  }

  ihs::log::info(
      "[DrmCompositor] direct-overlay mode: primary(BG)={} overlay(BS)={} "
      "(zpos={}), CRTC {}x{}",
      framed_primary_id_, direct_overlay_id_, direct_overlay_zpos_,
      out_.mode_width(), out_.mode_height());
  return true;
}

bool DrmCompositor::PresentDirectOverlay(const FlutterLayer** layers,
                                         const size_t count) {
  const bool profile = ProfileEnabled();
  const uint64_t t0 = profile ? NsNow() : 0;

  if (!WaitForPendingFlip()) {
    return PresentViaGlFallback(layers, count);
  }
  const uint64_t t1 = profile ? NsNow() : 0;

  // Find the single backing-store layer. Any platform-view layer, or
  // anything other than exactly one BS layer, forces the GL fallback —
  // multi-layer / PV direct-overlay is a follow-on.
  GbmBackingStore* store = nullptr;
  size_t bs_layers = 0;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* fl = layers[i];
    if (!fl) {
      continue;
    }
    if (fl->type == kFlutterLayerContentTypePlatformView) {
      return PresentViaGlFallback(layers, count);
    }
    if (fl->type == kFlutterLayerContentTypeBackingStore && fl->backing_store) {
      ++bs_layers;
      if (auto* baton =
              static_cast<StoreBaton*>(fl->backing_store->user_data)) {
        store = baton->store;
      }
    }
  }
  if (bs_layers != 1 || !store) {
    return PresentViaGlFallback(layers, count);
  }

  // KMS framebuffer on the BS BO's active slot. Bail to GL on failure.
  if (!EnsureDrmFbId(*store) || store->active().drm_fb_id == 0) {
    return PresentViaGlFallback(layers, count);
  }

  // Sync Flutter's GPU writes to the BS BO before the kernel scans it.
  // The scene path uses the same synchronous drain (IN_FENCE_FD on the
  // overlay is a follow-on).
  glFinish();

  // ── Build the atomic request ──
  drm::AtomicRequest req(backend_->device());
  if (!req.valid()) {
    ihs::log::warn(
        "[DrmCompositor] direct-overlay: AtomicRequest alloc failed");
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, count);
  }

  const uint32_t crtc_id = out_.crtc_id();
  const uint32_t mode_w = out_.mode_width();
  const uint32_t mode_h = out_.mode_height();
  const bool dump = backend_->cfg_.debug_backend && !plane_mode_set_;

  // First commit only: power up the pipe (MODE_ID / ACTIVE / CRTC_ID).
  if (!plane_mode_set_ && modeset_) {
    if (auto r = modeset_->attach(req); !r) {
      ihs::log::warn("[DrmCompositor] direct-overlay: modeset attach: {}",
                     r.error().message());
      return PresentViaGlFallback(layers, count);
    }
  }

  auto set = [&](const uint32_t obj, const char* name,
                 const uint64_t value) -> bool {
    auto pid = framed_props_.property_id(obj, name);
    if (!pid) {
      ihs::log::warn(
          "[DrmCompositor] direct-overlay: property {}.{} missing: {}", obj,
          name, pid.error().message());
      return false;
    }
    if (auto r = req.add_property(obj, *pid, value); !r) {
      ihs::log::warn("[DrmCompositor] direct-overlay: add_property {}.{}: {}",
                     obj, name, r.error().message());
      return false;
    }
    if (dump) {
      ihs::log::debug(
          "[DrmCompositor] direct-overlay prop: obj={} pid={} {}={}", obj, *pid,
          name, value);
    }
    return true;
  };

  // Primary: persistent mode-sized opaque BG covering the whole CRTC.
  if (!set(framed_primary_id_, "FB_ID", bg_store_.active().drm_fb_id) ||
      !set(framed_primary_id_, "CRTC_ID", crtc_id) ||
      !set(framed_primary_id_, "CRTC_X", 0) ||
      !set(framed_primary_id_, "CRTC_Y", 0) ||
      !set(framed_primary_id_, "CRTC_W", mode_w) ||
      !set(framed_primary_id_, "CRTC_H", mode_h) ||
      !set(framed_primary_id_, "SRC_X", 0) ||
      !set(framed_primary_id_, "SRC_Y", 0) ||
      !set(framed_primary_id_, "SRC_W", static_cast<uint64_t>(mode_w) << 16) ||
      !set(framed_primary_id_, "SRC_H", static_cast<uint64_t>(mode_h) << 16)) {
    return PresentViaGlFallback(layers, count);
  }
  if (auto pid = framed_props_.property_id(framed_primary_id_, "zpos")) {
    if (const auto immut =
            framed_props_.is_immutable(framed_primary_id_, "zpos");
        !immut || !*immut) {
      (void)req.add_property(framed_primary_id_, *pid, primary_zpos_);
    }
  }

  // Overlay: the Flutter backing store, direct-scanned over the full
  // CRTC with REFLECT_Y (the overlay supports it — that's the point).
  const uint64_t src_w = static_cast<uint64_t>(store->width) << 16;
  const uint64_t src_h = static_cast<uint64_t>(store->height) << 16;
  if (!set(direct_overlay_id_, "FB_ID", store->active().drm_fb_id) ||
      !set(direct_overlay_id_, "CRTC_ID", crtc_id) ||
      !set(direct_overlay_id_, "CRTC_X", 0) ||
      !set(direct_overlay_id_, "CRTC_Y", 0) ||
      !set(direct_overlay_id_, "CRTC_W", mode_w) ||
      !set(direct_overlay_id_, "CRTC_H", mode_h) ||
      !set(direct_overlay_id_, "SRC_X", 0) ||
      !set(direct_overlay_id_, "SRC_Y", 0) ||
      !set(direct_overlay_id_, "SRC_W", src_w) ||
      !set(direct_overlay_id_, "SRC_H", src_h) ||
      !set(direct_overlay_id_, "rotation",
           DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y)) {
    return PresentViaGlFallback(layers, count);
  }
  if (auto pid = framed_props_.property_id(direct_overlay_id_, "zpos")) {
    if (const auto immut =
            framed_props_.is_immutable(direct_overlay_id_, "zpos");
        !immut || !*immut) {
      (void)req.add_property(direct_overlay_id_, *pid, direct_overlay_zpos_);
    }
  }

  // Disable every other non-cursor plane so the commit doesn't inherit
  // stale FB/CRTC/zpos from fbcon or a prior session.
  for (const auto* p : plane_registry_->for_crtc(out_.crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    if (p->id == framed_primary_id_ || p->id == direct_overlay_id_) {
      continue;
    }
    if (auto fb_pid = framed_props_.property_id(p->id, "FB_ID")) {
      (void)req.add_property(p->id, *fb_pid, 0);
    }
    if (auto crtc_pid = framed_props_.property_id(p->id, "CRTC_ID")) {
      (void)req.add_property(p->id, *crtc_pid, 0);
    }
  }

  // ── Commit ──
  if (backend_->cfg_.debug_backend) {
    backend_->flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }

  // Stage the HW cursor into this commit (rides the same flip; see
  // StageCursorInto); its first plane-enable forces a blocking frame.
  const bool was_first_commit_pre = !plane_mode_set_;
  // `blocking` also covers a cursor first-enable; `was_first_commit_pre`
  // stays "initial modeset" for the slot-pipeline bookkeeping below.
  const bool blocking = was_first_commit_pre || StageCursorInto(req);
  const uint32_t commit_flags =
      blocking ? DRM_MODE_ATOMIC_ALLOW_MODESET
               : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);
  const uint64_t t2 = profile ? NsNow() : 0;

  if (auto r = req.commit(commit_flags, static_cast<IFlipSink*>(this)); !r) {
    if (r.error() == std::errc::permission_denied) {
      if (!paused_.load(std::memory_order_acquire)) {
        ihs::log::warn(
            "[DrmCompositor] direct-overlay commit: master revoked (EACCES); "
            "skip frame");
      }
      return false;
    }
    ihs::log::warn(
        "[DrmCompositor] direct-overlay atomic commit failed ({}); latching GL "
        "fallback for remaining session",
        r.error().message());
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, count);
  }

  SettleAtomicCommit(blocking);

  // Rotate the BS pool slot so Flutter's next render targets a different
  // BO than the one the kernel is scanning out. Same slot-release
  // pipeline bookkeeping as the scene path.
  if (store->pool_size > 1) {
    const size_t committed_idx = store->active_idx;
    store->pool[committed_idx].state = GbmBackingStore::Slot::State::Pending;
    SlotRef committed{store, committed_idx};
    store->active_idx = (store->active_idx + 1) % store->pool_size;
    store->pool[store->active_idx].state = GbmBackingStore::Slot::State::Active;
    glBindFramebuffer(GL_FRAMEBUFFER, store->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           store->active().color_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::lock_guard<std::mutex> lock(slot_pipeline_mu_);
    if (was_first_commit_pre) {
      for (auto& ref : scanning_slots_) {
        ref.store->pool[ref.slot_idx].state =
            GbmBackingStore::Slot::State::Free;
      }
      scanning_slots_.assign(1, committed);
    } else {
      in_flight_slots_.push_back({committed});
    }
  }

  if (profile && profile_) {
    const uint64_t t3 = NsNow();
    profile_->p.account(t1 - t0, t2 - t1, /*test=*/0, t3 - t2, t3 - t0);
    if (profile_->p.frames >= kProfileWindow) {
      const auto& s = profile_->p;
      const auto ms = [&](uint64_t ns_sum) {
        return static_cast<double>(ns_sum) /
               (static_cast<double>(s.frames) * 1e6);
      };
      const auto ms_max = [](uint64_t ns) {
        return static_cast<double>(ns) / 1e6;
      };
      ihs::log::info(
          "[DrmCompositor] direct-overlay profile (n={}): wait={:.2f}ms (max "
          "{:.2f})  sync={:.2f}ms (max {:.2f})  commit={:.2f}ms (max {:.2f})  "
          "total={:.2f}ms (max {:.2f})",
          s.frames, ms(s.wait_sum_ns), ms_max(s.wait_max_ns),
          ms(s.compose_sum_ns), ms_max(s.compose_max_ns), ms(s.commit_sum_ns),
          ms_max(s.commit_max_ns), ms(s.total_sum_ns), ms_max(s.total_max_ns));
      profile_->p.reset();
    }
  }
  return true;
}
#endif  // USE_DRM_SCENE

// ─── Backing store create / collect ──────────────────────────────────────

bool DrmCompositor::CreateBackingStore(const FlutterBackingStoreConfig* config,
                                       FlutterBackingStore* out) {
  EnsureGlCapsProbed();

  const auto w = static_cast<uint32_t>(config->size.width);
  const auto h = static_cast<uint32_t>(config->size.height);

  // Flutter backing stores must carry an alpha channel: Skia draws
  // transparent pixels over platform-view cutouts, and the top overlay
  // BS is composited with GL_ONE/GL_ONE_MINUS_SRC_ALPHA. An alpha-less
  // (X-bits) backing store would sample those unused bits as 0xFF and
  // paint opaque black over the PVs. driver_probe picks the alpha
  // sibling of primary_format when the primary plane advertises it in
  // IN_FORMATS, and falls back to primary_format otherwise so
  // direct-scanout still works on drivers whose primary is alpha-less.
  const uint32_t backing_format = out_.resolved().bs_format;

  std::unique_ptr<GbmBackingStore> store;
  for (auto it = store_pool_.begin(); it != store_pool_.end(); ++it) {
    if (*it && (*it)->width == w && (*it)->height == h &&
        (*it)->format == backing_format) {
      store = std::move(*it);
      store_pool_.erase(it);
      ++store_pool_hits_;
      break;
    }
  }
  if (!store) {
    store = std::make_unique<GbmBackingStore>();
    // Only multi-buffer when direct-scanout is actually reachable.
    // GL composite reads the BS BO as a texture (read-only during the
    // composite step) and writes into comp.fbo, which is already
    // multi-buffered at the comp_idx_ level — so the BS recycle race
    // doesn't exist on that path and the extra BOs are pure memory
    // tax. On Tegra with the nvidia-drm gate active this would 3×
    // the GBM footprint of image-heavy bundles (image_list,
    // wonderous, …) and visibly regress steady-state cadence via
    // GL-driver memory pressure.
    const size_t pool_size =
        any_plane_supports_reflect_y_ ? kFlutterBsPoolSize : 1;
    if (!CreateGbmStore(*store, w, h, backing_format, pool_size)) {
      return false;
    }
    ++store_pool_misses_;
  }

  auto* baton = new StoreBaton{};
  baton->owner = this;
  baton->store = store.get();
#if USE_DRM_SCENE
  // Pre-build the LayerBufferSource wrapper for the LayerScene path.
  // It stays in the baton until the first PresentLayers under
  // USE_DRM_SCENE moves it into scene_->add_layer(). If the store
  // retires (CollectBackingStore) before any Present, the wrapper is
  // freed with the baton.
  if (scene_) {
    baton->scene_source = std::make_unique<GbmBackingStoreLayerSource>(
        store.get(), [this](GbmBackingStore& s) { return EnsureDrmFbId(s); });
  }
#endif

  out->struct_size = sizeof(FlutterBackingStore);
  out->type = kFlutterBackingStoreTypeOpenGL;
  out->user_data = baton;
  out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  out->open_gl.framebuffer.target =
      gl_caps_.has_rgb8_rgba8 ? GL_RGBA8_OES : GL_RGBA;
  out->open_gl.framebuffer.name = store->fbo;
  out->open_gl.framebuffer.user_data = baton;
  out->open_gl.framebuffer.destruction_callback = [](void*) {};

  stores_[baton] = std::move(store);

  ++store_create_total_;
  if (stores_.size() > store_peak_live_) {
    store_peak_live_ = stores_.size();
  }
  if (backend_->cfg_.debug_backend) {
    ihs::log::debug(
        "[DrmCompositor] CreateBackingStore {}x{} #{} live={} peak={} "
        "pool(hit={} miss={} idle={})",
        w, h, store_create_total_, stores_.size(), store_peak_live_,
        store_pool_hits_, store_pool_misses_, store_pool_.size());
  }
  return true;
}

DrmCompositor* DrmCompositor::OwnerOf(const FlutterBackingStore* store) {
  if (store == nullptr || store->user_data == nullptr) {
    return nullptr;
  }
  return static_cast<StoreBaton*>(store->user_data)->owner;
}

bool DrmCompositor::CollectBackingStore(const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
#if USE_DRM_SCENE
  // If the store appeared in a Present this generation, the LayerScene
  // owns a Layer with identity_tag == baton. Drop it before the baton
  // disappears so the scene doesn't leave a stale layer referring to
  // memory we're about to free.
  if (scene_) {
    if (auto* layer = scene_->find_by_identity_tag(baton)) {
      scene_->remove_layer(layer->handle());
    }
  }
#endif
  if (const auto it = stores_.find(baton); it != stores_.end()) {
    // Return the store to the pool instead of destroying it. Keeps the
    // GBM BO + EGLImage + GL FBO/tex/RB and (if already imported) the
    // KMS FB alive for the next matching CreateBackingStore.
    store_pool_.push_back(std::move(it->second));
    stores_.erase(it);
  }
  delete baton;

  ++store_collect_total_;
  if (backend_->cfg_.debug_backend) {
    ihs::log::debug("[DrmCompositor] CollectBackingStore #{} live={} idle={}",
                    store_collect_total_, stores_.size(), store_pool_.size());
  }
  return true;
}

// ─── Session pause / resume ──────────────────────────────────────────────

void DrmCompositor::ReserveCursorPlane(const uint32_t plane_id) {
  cursor_reserved_plane_ = plane_id;
  // Apply immediately in case the scene already exists (cursor wired
  // after the first present); otherwise the scene-build path applies it.
  ApplyCursorReservation();
}

void DrmCompositor::ApplyCursorReservation() {
#if USE_DRM_SCENE
  if (!scene_ || cursor_reserved_plane_ == 0) {
    return;
  }
  const uint32_t planes[] = {cursor_reserved_plane_};
  scene_->set_external_reserved_planes(drm::span<const uint32_t>(planes, 1));
  ihs::log::info(
      "[DrmCompositor] reserved cursor plane {} from scene allocator "
      "(disable-unused pass will leave it alone)",
      cursor_reserved_plane_);
#endif
}

void DrmCompositor::SetPaused(const bool paused) {
  paused_.store(paused, std::memory_order_release);
#if USE_DRM_SCENE
  // Mirror the pause edge into the scene so its layer sources can
  // drop fd-bound state before the kernel revokes master. Idempotent
  // and infallible per the LayerBufferSource::on_session_paused
  // contract.
  if (paused && scene_) {
    scene_->on_session_paused();
  }
#endif
  ihs::log::info("[DrmCompositor] session {}", paused ? "paused" : "resumed");
}

void DrmCompositor::OnResume() {
  // The kernel revoked master while we were inactive; whatever flip
  // we had outstanding never fired PAGE_FLIP_EVENT, and on amdgpu the
  // CRTC state can be re-asserted by re-issuing the mode. Clear the
  // latches so the next PresentLayers does a full mode-set, the same
  // way the first commit after Create() does.
  flip_pending_.store(false, std::memory_order_release);
  plane_mode_set_ = false;
  paused_.store(false, std::memory_order_release);
  resume_pending_logged_ = false;

#if USE_DRM_SCENE
  // Rebuild the scene's per-fd kernel state (plane registry, property
  // caches, GEM/FB handles). DrmSession::TakeDevice opts in to
  // preserve_fd_across_resume, so backend_->device() wraps the same
  // fd that the seat originally handed us — the LayerScene's
  // implementation skips fd-substitution and just re-enumerates.
  if (scene_) {
    auto& dev = const_cast<drm::Device&>(backend_->device());
    if (auto r = scene_->on_session_resumed(dev); !r) {
      ihs::log::error("[DrmCompositor] LayerScene::on_session_resumed: {}",
                      r.error().message());
    }
  }
#endif

  // Drain the BS-slot release pipeline — any flip events that would
  // have freed slots queued before pause never fired. Promote
  // everything to Free so the next render finds usable slots.
  {
    std::lock_guard<std::mutex> lock(slot_pipeline_mu_);
    for (auto& ref : scanning_slots_) {
      ref.store->pool[ref.slot_idx].state = GbmBackingStore::Slot::State::Free;
    }
    scanning_slots_.clear();
    while (!in_flight_slots_.empty()) {
      for (auto& ref : in_flight_slots_.front()) {
        ref.store->pool[ref.slot_idx].state =
            GbmBackingStore::Slot::State::Free;
      }
      in_flight_slots_.pop_front();
    }
  }

  ihs::log::info("[DrmCompositor] resumed — next commit will re-modeset");
}

// ─── Surface registry ────────────────────────────────────────────────────

void DrmCompositor::RegisterSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  std::lock_guard lock(surfaces_mu_);
  surfaces_[id] = std::move(surface);
  if (backend_->cfg_.debug_backend) {
    ihs::log::debug("[DrmCompositor] RegisterSurface id={} registry_size={}",
                    id, surfaces_.size());
  }
}

void DrmCompositor::UnregisterSurface(FlutterPlatformViewIdentifier id) {
  std::lock_guard lock(surfaces_mu_);
  surfaces_.erase(id);
  if (backend_->cfg_.debug_backend) {
    ihs::log::debug("[DrmCompositor] UnregisterSurface id={} registry_size={}",
                    id, surfaces_.size());
  }
}

void DrmCompositor::ResizeSurface(FlutterPlatformViewIdentifier id,
                                  int32_t width,
                                  int32_t height) {
  std::shared_ptr<ICompositorSurface> surface_sp;
  {
    std::lock_guard lock(surfaces_mu_);
    if (const auto it = surfaces_.find(id); it != surfaces_.end()) {
      surface_sp = it->second;
    }
  }
  if (surface_sp) {
    surface_sp->OnResize(width, height);
  }
}
