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

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm-cxx/modeset/atomic.hpp>

#include "backend/drm_kms_egl/driver_probe.h"
#include "backend/drm_kms_egl/drm_backend.h"
#include "libflutter_engine.h"
#include "logging.h"
#include "view/compositor_surface_interface.h"

namespace {

constexpr int kPollTimeoutMs = 100;

bool AllocDebug() {
  static const bool enabled = std::getenv("DRM_ALLOC_DEBUG") != nullptr;
  return enabled;
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
    std::fprintf(
        stderr,
        "[drm-cxx] snapshot %s id=%u: getProperties failed (errno=%d)\n", label,
        obj_id, errno);
    return;
  }
  std::fprintf(stderr, "[drm-cxx] snapshot %s id=%u (%u props):\n", label,
               obj_id, props->count_props);
  for (uint32_t i = 0; i < props->count_props; ++i) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (prop == nullptr) {
      continue;
    }
    const uint64_t value = props->prop_values[i];
    const char* kind = "RANGE";
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
    std::fprintf(stderr, "[drm-cxx]   %-20s id=%u value=%llu [%s]\n",
                 prop->name, prop->prop_id,
                 static_cast<unsigned long long>(value), kind);
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

DrmCompositor::DrmCompositor(DrmBackend* backend) : backend_(backend) {}

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
  return eglCreateImageKHR_ && eglDestroyImageKHR_ &&
         glEGLImageTargetTexture2DOES_;
}

bool DrmCompositor::InitPlaneAllocator() {
  auto reg = drm::planes::PlaneRegistry::enumerate(backend_->device());
  if (!reg) {
    spdlog::warn("[DrmCompositor] PlaneRegistry: {}", reg.error().message());
    return false;
  }
  plane_registry_.emplace(std::move(*reg));

  const auto available = plane_registry_->for_crtc(backend_->crtc_index());
  spdlog::info("[DrmCompositor] {} planes available for CRTC {}",
               available.size(), backend_->crtc_id());
  for (const auto* p : available) {
    const char* type_str = "?";
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
    spdlog::info("[DrmCompositor]   plane {} type={} zpos=[{},{}]", p->id,
                 type_str, p->zpos_min.value_or(-1), p->zpos_max.value_or(-1));
    if (p->type == drm::planes::DRMPlaneType::PRIMARY) {
      // Use zpos_min: when immutable it's the only legal value; when
      // mutable it's still the lowest slot, and we want the root Flutter
      // layer on the bottom.
      primary_zpos_ = p->zpos_min.value_or(0);
    }
  }
  spdlog::info("[DrmCompositor] primary plane zpos = {}", primary_zpos_);

  // Pre-commit snapshot: what fbcon/prior session left on the pipe.
  // EINVAL on the first atomic TEST usually correlates to a residual
  // property value (cursor bound to CRTC, stale MODE_ID blob, non-default
  // color encoding, etc.) that we don't overwrite. Dumping once at init
  // lets the next failed-TEST diff against a known starting point.
  if (AllocDebug()) {
    const int fd = backend_->drm_fd();
    DumpObjectProps(fd, backend_->crtc_id(), DRM_MODE_OBJECT_CRTC, "CRTC");
    DumpObjectProps(fd, backend_->connector_id(), DRM_MODE_OBJECT_CONNECTOR,
                    "Connector");
    for (const auto& p : plane_registry_->all()) {
      char label[32];
      std::snprintf(label, sizeof(label), "Plane[%s]", PlaneTypeName(p.type));
      DumpObjectProps(fd, p.id, DRM_MODE_OBJECT_PLANE, label);
    }
  }

  auto ms =
      drm::modeset::Modeset::create(backend_->device(), backend_->crtc_id(),
                                    backend_->connector_id(), backend_->mode());
  if (!ms) {
    spdlog::error("[DrmCompositor] Modeset::create: {}", ms.error().message());
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
  framed_ = (backend_->width() != backend_->mode_width()) ||
            (backend_->height() != backend_->mode_height());
  if (framed_ && !InitFramedMode()) {
    spdlog::error(
        "[DrmCompositor] framed-mode init failed; plane path disabled");
    return false;
  }
  return true;
}

bool DrmCompositor::InitFramedMode() {
  // Pick the primary plane (already selected by the allocator's zpos
  // lookup) and the first overlay plane whose zpos range straddles
  // primary_zpos_ + 1 and that advertises the composition buffer's
  // scanout format. The overlay scans out the framed content; the
  // primary scans out a mode-sized opaque BG for the remainder of the
  // CRTC so amdgpu DC's "primary must cover CRTC" check is satisfied.
  const uint32_t comp_format = backend_->resolved().primary_format;
  const uint64_t want_overlay_zpos = primary_zpos_ + 1;

  for (const auto* p : plane_registry_->for_crtc(backend_->crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::PRIMARY &&
        framed_primary_id_ == 0) {
      framed_primary_id_ = p->id;
      continue;
    }
    if (p->type == drm::planes::DRMPlaneType::OVERLAY &&
        framed_overlay_id_ == 0) {
      if (!p->supports_format(comp_format)) {
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
    spdlog::error(
        "[DrmCompositor] framed mode needs 1 primary + 1 overlay plane on "
        "CRTC {} (primary={}, overlay={})",
        backend_->crtc_id(), framed_primary_id_, framed_overlay_id_);
    return false;
  }

  // Cache property IDs for both planes so PresentFramed can build the
  // atomic request without repeated kernel round-trips.
  const int fd = backend_->drm_fd();
  if (auto r = framed_props_.cache_properties(fd, framed_primary_id_,
                                              DRM_MODE_OBJECT_PLANE);
      !r) {
    spdlog::error("[DrmCompositor] cache primary props: {}",
                  r.error().message());
    return false;
  }
  if (auto r = framed_props_.cache_properties(fd, framed_overlay_id_,
                                              DRM_MODE_OBJECT_PLANE);
      !r) {
    spdlog::error("[DrmCompositor] cache overlay props: {}",
                  r.error().message());
    return false;
  }
  // Also cache non-cursor planes we'll need to disable each frame, so
  // their FB_ID/CRTC_ID IDs are resolvable without touching the kernel.
  for (const auto* p : plane_registry_->for_crtc(backend_->crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    if (p->id == framed_primary_id_ || p->id == framed_overlay_id_) {
      continue;
    }
    (void)framed_props_.cache_properties(fd, p->id, DRM_MODE_OBJECT_PLANE);
  }

  spdlog::info(
      "[DrmCompositor] framed mode: primary={} overlay={} (zpos={}), "
      "BG {}x{}, content {}x{} at ({},{})",
      framed_primary_id_, framed_overlay_id_, framed_overlay_zpos_,
      backend_->mode_width(), backend_->mode_height(), backend_->width(),
      backend_->height(), (backend_->mode_width() - backend_->width()) / 2,
      (backend_->mode_height() - backend_->height()) / 2);
  return true;
}

bool DrmCompositor::InitCompositionBuffers() {
  // Composition buffer scans out on the primary plane, so its GBM format
  // must match what the primary accepts — use the resolved format, not a
  // hardcoded constant. (XRGB on most drivers, XBGR on e.g. tilcdc.)
  const uint32_t format = backend_->resolved().primary_format;
  for (auto& comp_buf : comp_bufs_) {
    if (!CreateGbmStore(comp_buf, backend_->width(), backend_->height(),
                        format) ||
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
    if (!CreateGbmStore(bg_store_, backend_->mode_width(),
                        backend_->mode_height(), format) ||
        !EnsureDrmFbId(bg_store_)) {
      spdlog::error("[DrmCompositor] BG GBM store create failed");
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
    spdlog::error("[DrmCompositor] missing EGL_KHR_image / GL_OES_EGL_image");
  }

  // Only take the plane path if DriverProbe said this driver supports it
  // (atomic + primary + ≥1 overlay). Otherwise all frames go through
  // PresentViaGlFallback — same outcome as before, just honest about it.
  if (backend_->resolved().use_plane_compositor) {
    if (!InitPlaneAllocator()) {
      spdlog::warn("[DrmCompositor] plane allocator init failed; GL fallback");
    } else {
      // Flip planes_available_ BEFORE InitCompositionBuffers so the
      // CreateGbmStore call-chain inside it imports each comp BO as a
      // KMS FB. Otherwise comp.drm_fb_id is 0 and the atomic commit
      // tells the kernel to disable the primary plane — blank screen.
      planes_available_ = true;
      if (InitCompositionBuffers()) {
        spdlog::info("[DrmCompositor] plane allocator active");
      } else {
        planes_available_ = false;
        spdlog::warn(
            "[DrmCompositor] composition buffer init failed; GL fallback");
      }
    }
  } else {
    spdlog::info("[DrmCompositor] plane compositor disabled by probe; GL path");
  }
  gl_caps_probed_ = true;
}

// ─── GBM store create / destroy ──────────────────────────────────────────

bool DrmCompositor::CreateGbmStore(GbmBackingStore& store,
                                   uint32_t w,
                                   uint32_t h,
                                   const uint32_t format) const {
  store.width = w;
  store.height = h;
  store.format = format;
  const uint32_t usage = planes_available_
                             ? (GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT)
                             : GBM_BO_USE_RENDERING;
  store.bo = gbm_bo_create(backend_->gbm(), w, h, format, usage);
  if (!store.bo) {
    spdlog::error("[DrmCompositor] gbm_bo_create {}x{}: {}", w, h,
                  std::strerror(errno));
    return false;
  }

  store.egl_image = eglCreateImageKHR_(
      backend_->egl_display(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
      reinterpret_cast<EGLClientBuffer>(store.bo), nullptr);
  if (store.egl_image == EGL_NO_IMAGE_KHR) {
    spdlog::error("[DrmCompositor] eglCreateImageKHR: 0x{:x}", eglGetError());
    DestroyGbmStore(store);
    return false;
  }

  glGenTextures(1, &store.color_tex);
  glBindTexture(GL_TEXTURE_2D, store.color_tex);
  glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D,
                                static_cast<GLeglImageOES>(store.egl_image));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &store.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, store.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         store.color_tex, 0);

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
    spdlog::error("[DrmCompositor] FBO incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    DestroyGbmStore(store);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // KMS framebuffer import is deferred to EnsureDrmFbId(). Callers that
  // immediately scan out this BO (comp_bufs_, bg_store_) must invoke
  // EnsureDrmFbId() after construction; Flutter-side backing stores
  // let the layer-setup path import on first direct-scanout use.
  return true;
}

bool DrmCompositor::EnsureDrmFbId(GbmBackingStore& store) const {
  if (store.drm_fb_id != 0) {
    return true;
  }
  if (!store.bo) {
    return false;
  }
  store.drm_fb_id = ImportBoAsFb(store.bo);
  return store.drm_fb_id != 0;
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
      spdlog::error(
          "[DrmCompositor] drmModeAddFB2WithModifiers({}x{}, fmt=0x{:08x}, "
          "mod=0x{:016x}, planes={}): {}",
          w, h, format, modifier, plane_count, std::strerror(errno));
      return 0;
    }
    if (backend_->cfg_.debug_backend) {
      spdlog::debug(
          "[DrmCompositor] AddFB2WithModifiers fb_id={} {}x{} fmt=0x{:08x} "
          "mod=0x{:016x} planes={}",
          fb_id, w, h, format, modifier, plane_count);
    }
  } else {
    if (drmModeAddFB2(backend_->drm_fd(), w, h, format, handles, pitches,
                      offsets, &fb_id, 0) != 0) {
      spdlog::error(
          "[DrmCompositor] drmModeAddFB2({}x{}, fmt=0x{:08x}, linear): {}", w,
          h, format, std::strerror(errno));
      return 0;
    }
    if (backend_->cfg_.debug_backend) {
      spdlog::debug("[DrmCompositor] AddFB2 fb_id={} {}x{} fmt=0x{:08x} linear",
                    fb_id, w, h, format);
    }
  }
  return fb_id;
}

void DrmCompositor::DestroyGbmStore(GbmBackingStore& s) const {
  if (s.fbo != 0) {
    glDeleteFramebuffers(1, &s.fbo);
    s.fbo = 0;
  }
  if (s.color_tex != 0) {
    glDeleteTextures(1, &s.color_tex);
    s.color_tex = 0;
  }
  if (s.depth_stencil_rb != 0) {
    glDeleteRenderbuffers(1, &s.depth_stencil_rb);
    s.depth_stencil_rb = 0;
  }
  if (s.egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_) {
    eglDestroyImageKHR_(backend_->egl_display(), s.egl_image);
    s.egl_image = EGL_NO_IMAGE_KHR;
  }
  if (s.drm_fb_id != 0) {
    drmModeRmFB(backend_->drm_fd(), s.drm_fb_id);
    s.drm_fb_id = 0;
  }
  if (s.bo) {
    gbm_bo_destroy(s.bo);
    s.bo = nullptr;
  }
}

// ─── Page-flip synchronisation ───────────────────────────────────────────

void DrmCompositor::PageFlipHandler(int /*fd*/,
                                    unsigned int /*seq*/,
                                    unsigned int /*tv_sec*/,
                                    unsigned int /*tv_usec*/,
                                    void* user_data) {
  auto* self = static_cast<DrmCompositor*>(user_data);
  self->flip_pending_ = false;
  self->backend_->RecordFlipComplete();

  // Return the vsync baton now that the kernel has confirmed scanout.
  // Mirrors DrmBackend::PageFlipHandler (legacy path) so the steady-state
  // flip cadence drives Flutter's scheduler in plane mode too.
  const intptr_t baton =
      self->backend_->vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
  auto engine = self->backend_->vsync_engine_.load(std::memory_order_relaxed);
  if (!engine) {
    return;
  }
  const uint64_t now = LibFlutterEngine->GetCurrentTime();
  const uint64_t period_ns =
      self->backend_->vrefresh() > 0
          ? 1000000000ULL / static_cast<uint64_t>(self->backend_->vrefresh())
          : 16666667ULL;
  LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
}

bool DrmCompositor::WaitForPendingFlip() const {
  if (!flip_pending_) {
    return true;
  }
  drmEventContext ctx{};
  ctx.version = 2;
  ctx.page_flip_handler = &DrmCompositor::PageFlipHandler;

  while (flip_pending_) {
    pollfd pfd{};
    pfd.fd = backend_->drm_fd();
    pfd.events = POLLIN;
    const int r = poll(&pfd, 1, kPollTimeoutMs);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[DrmCompositor] poll: {}", std::strerror(errno));
      return false;
    }
    if (r > 0 && (pfd.revents & POLLIN)) {
      drmHandleEvent(backend_->drm_fd(), &ctx);
    }
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
                                         size_t count) {
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
            s->fbo, s->color_tex, static_cast<GLsizei>(s->width),
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
          const auto dy = static_cast<GLint>(backend_->height()) -
                          static_cast<GLint>(layer->offset.y) - dh;
          gl_compositor_->CompositeToDefault(0, tex, sw, sh, dx, dy, dw, dh,
                                             blend, true);
          composited_any = true;
        }
      }
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return backend_->Present();
}

// ─── Framed-mode present (primary BG + overlay content) ─────────────────

bool DrmCompositor::PresentFramed(const FlutterLayer** layers,
                                  const size_t layer_count) {
  if (!WaitForPendingFlip()) {
    return PresentViaGlFallback(layers, layer_count);
  }

  // Composite every Flutter layer into the current comp buffer. Same
  // pixel work as PresentViaGlFallback, but the destination is the GBM
  // BO we'll scan out on the overlay rather than FBO 0.
  auto& comp = comp_bufs_[comp_idx_];
  glBindFramebuffer(GL_FRAMEBUFFER, comp.fbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  bool composited_any = false;
  for (size_t i = 0; i < layer_count; ++i) {
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
        CompositeLayerIntoFbo(comp.fbo, s->fbo, s->color_tex,
                              static_cast<GLsizei>(s->width),
                              static_cast<GLsizei>(s->height),
                              static_cast<GLint>(layer->offset.x),
                              static_cast<GLint>(layer->offset.y),
                              static_cast<GLsizei>(layer->size.width),
                              static_cast<GLsizei>(layer->size.height), blend,
                              /*flip_y=*/true);
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
          CompositeLayerIntoFbo(comp.fbo, /*src_fbo=*/0, tex,
                                surface_sp->GetGlTextureWidth(),
                                surface_sp->GetGlTextureHeight(),
                                static_cast<GLint>(layer->offset.x),
                                static_cast<GLint>(layer->offset.y),
                                static_cast<GLsizei>(layer->size.width),
                                static_cast<GLsizei>(layer->size.height), blend,
                                /*flip_y=*/true);
          composited_any = true;
        }
      }
    }
  }
  glFinish();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (backend_->cfg_.debug_backend) {
    spdlog::debug(
        "[DrmCompositor] framed frame: layers={} composited={} comp_idx={} "
        "comp_fb={}",
        layer_count, composited_any, comp_idx_, comp.drm_fb_id);
  }

  // ── Build the atomic request ──

  drm::AtomicRequest req(backend_->device());
  if (!req.valid()) {
    spdlog::warn("[DrmCompositor] framed: AtomicRequest alloc failed");
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, layer_count);
  }

  const uint32_t crtc_id = backend_->crtc_id();
  const uint32_t mode_w = backend_->mode_width();
  const uint32_t mode_h = backend_->mode_height();
  const uint32_t fb_w = backend_->width();
  const uint32_t fb_h = backend_->height();
  const int32_t lx = static_cast<int32_t>(mode_w - fb_w) / 2;
  const int32_t ly = static_cast<int32_t>(mode_h - fb_h) / 2;

  const bool dump = backend_->cfg_.debug_backend && !plane_mode_set_;
  if (dump) {
    spdlog::debug(
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
      spdlog::warn("[DrmCompositor] framed: modeset attach: {}",
                   r.error().message());
      return PresentViaGlFallback(layers, layer_count);
    }
    if (dump) {
      spdlog::debug(
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
      spdlog::warn("[DrmCompositor] framed: property {}.{} missing: {}", obj,
                   name, pid.error().message());
      return false;
    }
    if (auto r = req.add_property(obj, *pid, value); !r) {
      spdlog::warn("[DrmCompositor] framed: add_property {}.{}: {}", obj, name,
                   r.error().message());
      return false;
    }
    if (dump) {
      spdlog::debug("[DrmCompositor] framed prop: obj={} pid={} {}={}", obj,
                    *pid, name, value);
    }
    return true;
  };

  auto log_raw = [&](const uint32_t obj, const uint32_t pid, const char* name,
                     const uint64_t value) {
    if (dump) {
      spdlog::debug("[DrmCompositor] framed prop: obj={} pid={} {}={}", obj,
                    pid, name, value);
    }
  };

  // Primary: persistent mode-sized opaque BG covering the whole CRTC.
  // Kept on every frame so amdgpu DC sees "primary covers CRTC".
  if (!set(framed_primary_id_, "FB_ID", bg_store_.drm_fb_id) ||
      !set(framed_primary_id_, "CRTC_ID", crtc_id) ||
      !set(framed_primary_id_, "CRTC_X", 0) ||
      !set(framed_primary_id_, "CRTC_Y", 0) ||
      !set(framed_primary_id_, "CRTC_W", mode_w) ||
      !set(framed_primary_id_, "CRTC_H", mode_h) ||
      !set(framed_primary_id_, "SRC_X", 0) ||
      !set(framed_primary_id_, "SRC_Y", 0) ||
      !set(framed_primary_id_, "SRC_W", static_cast<uint64_t>(mode_w) << 16) ||
      !set(framed_primary_id_, "SRC_H", static_cast<uint64_t>(mode_h) << 16)) {
    return PresentViaGlFallback(layers, layer_count);
  }
  // zpos may be immutable on some drivers (amdgpu primary = [2,2]).
  // The kernel rejects any atomic write to an IMMUTABLE property with
  // -EINVAL regardless of value, so skip the write when the cached flags
  // say it's immutable. Missing property is benign.
  if (auto pid = framed_props_.property_id(framed_primary_id_, "zpos")) {
    const auto immut = framed_props_.is_immutable(framed_primary_id_, "zpos");
    if (!immut || !*immut) {
      (void)req.add_property(framed_primary_id_, *pid, primary_zpos_);
      log_raw(framed_primary_id_, *pid, "zpos", primary_zpos_);
    }
  }

  // Overlay: carries the composited content, centred on the CRTC.
  if (!set(framed_overlay_id_, "FB_ID", comp.drm_fb_id) ||
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
    return PresentViaGlFallback(layers, layer_count);
  }
  if (auto pid = framed_props_.property_id(framed_overlay_id_, "zpos")) {
    const auto immut = framed_props_.is_immutable(framed_overlay_id_, "zpos");
    if (!immut || !*immut) {
      (void)req.add_property(framed_overlay_id_, *pid, framed_overlay_zpos_);
      log_raw(framed_overlay_id_, *pid, "zpos", framed_overlay_zpos_);
    }
  }

  // Disable every other non-cursor plane on this CRTC so the commit
  // doesn't inherit stale FB/CRTC/zpos from fbcon or a prior session.
  for (const auto* p : plane_registry_->for_crtc(backend_->crtc_index())) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    if (p->id == framed_primary_id_ || p->id == framed_overlay_id_) {
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

  uint32_t commit_flags = 0;
  if (!plane_mode_set_) {
    commit_flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  } else {
    commit_flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
  }

  // Debug-only TEST_ONLY probe on the very first commit so a failed
  // real commit can be attributed to validation (EINVAL returned here)
  // vs. page-flip/driver dispatch. No side effect if the test succeeds.
  if (dump) {
    const uint32_t test_flags =
        DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (auto r = req.test(test_flags); !r) {
      spdlog::warn(
          "[DrmCompositor] framed TEST_ONLY commit failed ({}); real commit "
          "will likely fail with the same error",
          r.error().message());
    } else {
      spdlog::debug("[DrmCompositor] framed TEST_ONLY commit passed");
    }
  }

  if (dump) {
    spdlog::debug("[DrmCompositor] framed commit: flags=0x{:x}", commit_flags);
  }

  if (auto r = req.commit(commit_flags, this); !r) {
    spdlog::warn(
        "[DrmCompositor] framed atomic commit failed ({}); latching GL "
        "fallback",
        r.error().message());
    spdlog::error(
        "[DrmCompositor] framed config ({}x{} on {}x{} mode) requires atomic "
        "plane support; GL fallback cannot letterbox. Next Present will fail.",
        fb_w, fb_h, mode_w, mode_h);
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, layer_count);
  }

  if (!plane_mode_set_) {
    plane_mode_set_ = true;
    flip_pending_ = false;
    VerifyPipeRunning();

    const intptr_t baton =
        backend_->vsync_baton_.exchange(0, std::memory_order_acq_rel);
    if (baton != 0) {
      if (auto engine =
              backend_->vsync_engine_.load(std::memory_order_relaxed)) {
        const uint64_t now = LibFlutterEngine->GetCurrentTime();
        const uint64_t period_ns =
            backend_->vrefresh() > 0
                ? 1000000000ULL / static_cast<uint64_t>(backend_->vrefresh())
                : 16666667ULL;
        LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
      }
    }
  } else {
    flip_pending_ = true;
  }
  comp_idx_ ^= 1;
  return true;
}

// ─── Post-modeset pipe verification ──────────────────────────────────────

void DrmCompositor::VerifyPipeRunning() const {
  const int fd = backend_->drm_fd();
  const uint32_t crtc_id = backend_->crtc_id();

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
    spdlog::warn("[DrmCompositor] pipe-check: CRTC.ACTIVE property not found");
  } else if (crtc_active != 1) {
    spdlog::error(
        "[DrmCompositor] pipe-check: CRTC {} NOT active (ACTIVE={}) after "
        "modeset commit. The atomic commit reported success but the kernel "
        "did not activate the pipe — no PAGE_FLIP_EVENTs will fire.",
        crtc_id, crtc_active);
  } else {
    spdlog::info("[DrmCompositor] pipe-check: CRTC {} ACTIVE=1", crtc_id);
  }

  // ── Primary plane FB_ID readback ──
  uint32_t primary_plane_id = 0;
  if (plane_registry_) {
    for (const auto* p : plane_registry_->for_crtc(backend_->crtc_index())) {
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
          const std::string_view name(p->name);
          if (name == "FB_ID") {
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
      spdlog::error(
          "[DrmCompositor] pipe-check: primary plane {} has no FB_ID "
          "(found={}, value={}). Kernel silently accepted commit without "
          "binding a framebuffer — nothing to scan out.",
          primary_plane_id, fb_found, primary_fb);
    } else {
      spdlog::info(
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
    spdlog::warn(
        "[DrmCompositor] pipe-check: drmCrtcGetSequence unsupported or "
        "failed ({}); skipping vblank probe",
        std::strerror(errno));
    return;
  }
  const uint32_t vrefresh =
      backend_->vrefresh() > 0 ? backend_->vrefresh() : 60;
  const useconds_t sleep_us = 2'000'000u / vrefresh;  // 2 refresh periods
  ::usleep(sleep_us);
  uint64_t seq_after = 0;
  uint64_t ns_after = 0;
  if (drmCrtcGetSequence(fd, crtc_id, &seq_after, &ns_after) != 0) {
    spdlog::warn("[DrmCompositor] pipe-check: drmCrtcGetSequence (after): {}",
                 std::strerror(errno));
    return;
  }
  if (seq_after == seq_before) {
    spdlog::error(
        "[DrmCompositor] pipe-check: vblank sequence did NOT advance over "
        "~{} ms (seq={}). CRTC {} is not scanning out — PAGE_FLIP_EVENTs "
        "will never fire. Likely causes: connector DPMS off, wrong "
        "connector bound, primary format/modifier unsupported for scanout, "
        "or driver silent-accept bug.",
        sleep_us / 1000, seq_before, crtc_id);
  } else {
    spdlog::info(
        "[DrmCompositor] pipe-check: vblank advanced {} → {} ({} frames in "
        "~{} ms) — pipe is live",
        seq_before, seq_after, seq_after - seq_before, sleep_us / 1000);
  }
}

// ─── PresentLayers (plane-allocator path) ────────────────────────────────

bool DrmCompositor::PresentLayers(const FlutterLayer** layers,
                                  const size_t layer_count) {
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

  // Wait for any in-flight atomic commit before building the next frame.
  // Baton return is handled inside PageFlipHandler (vsync-locked) and
  // the first-frame path below, so no baton work is needed here.
  if (!WaitForPendingFlip()) {
    return PresentViaGlFallback(layers, layer_count);
  }

  // ── Build the drm-cxx Output with one Layer per Flutter layer ──

  drm::planes::Output output(backend_->crtc_id(), comp_layer_);

  // Letterbox offsets: Flutter layers carry offsets in FB coordinates,
  // but planes land on CRTC coordinates. When the FB is smaller than the
  // mode (framed config), every plane rect shifts by the same centering
  // delta so Flutter still sees a (0,0)-origin surface.
  const uint32_t fb_w = backend_->width();
  const uint32_t fb_h = backend_->height();
  const uint32_t mode_w = backend_->mode_width();
  const uint32_t mode_h = backend_->mode_height();
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

    // Direct-scanout requires a KMS framebuffer on the store's BO.
    // EnsureDrmFbId imports on first use and caches for the BO's
    // lifetime; pooled stores keep the fb_id across Collect → Create
    // cycles, so at steady state there are no AddFB2 calls per frame.
    if (store && EnsureDrmFbId(*store)) {
      // Layer 0 → primary plane (zpos = primary's immutable/min zpos,
      // queried at init). Subsequent layers stack above at
      // primary_zpos_ + i, which falls in the overlay zpos range on
      // every driver we've seen. Without this, a layer at zpos=0 on a
      // driver where primary's zpos is != 0 (e.g. amdgpu's [2,2]) lands
      // on an overlay and primary stays FB-less — blank screen.
      const uint64_t layer_zpos = primary_zpos_ + static_cast<uint64_t>(i);
      drm_layer.set_property("FB_ID", store->drm_fb_id)
          .set_property("CRTC_ID", backend_->crtc_id())
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
          .set_property("zpos", layer_zpos);
      drm_layer.set_content_type(i == 0 ? drm::planes::ContentType::UI
                                        : drm::planes::ContentType::Generic);
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
  comp_layer_.set_property("FB_ID", comp.drm_fb_id)
      .set_property("CRTC_ID", backend_->crtc_id())
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
      spdlog::warn("[DrmCompositor] modeset attach: {}; falling back to GL",
                   r.error().message());
      return PresentViaGlFallback(layers, layer_count);
    }
  }

  auto result = allocator_->apply(output, req, test_flags);
  if (!result) {
    spdlog::warn("[DrmCompositor] allocator: {}; falling back to GL",
                 result.error().message());
    return PresentViaGlFallback(layers, layer_count);
  }
  if (backend_->cfg_.debug_backend) {
    spdlog::info("[DrmCompositor] {} of {} layers on HW planes", *result,
                 frame_layers.size());
    for (size_t i = 0; i < frame_layers.size(); ++i) {
      const auto& fl = frame_layers[i];
      if (auto pid = fl.drm->assigned_plane_id()) {
        spdlog::info(
            "[DrmCompositor]   layer {} → plane {} (fb_id={}, {}x{}, zpos={})",
            i, *pid, fl.store ? fl.store->drm_fb_id : 0,
            fl.store ? fl.store->width : 0, fl.store ? fl.store->height : 0,
            primary_zpos_ + i);
      } else if (fl.drm->needs_composition()) {
        spdlog::info("[DrmCompositor]   layer {} → composition (overflow)", i);
      } else {
        spdlog::info("[DrmCompositor]   layer {} → unassigned?!", i);
      }
    }
    const auto comp_pid = comp_layer_.assigned_plane_id();
    spdlog::info(
        "[DrmCompositor]   comp_layer → {} (fb_id={})",
        comp_pid ? std::to_string(*comp_pid) : std::string("<unassigned>"),
        comp.drm_fb_id);
  }

  // ── GL-composite layers that overflowed into the composition buffer ──

  bool any_composited = false;
  glBindFramebuffer(GL_FRAMEBUFFER, comp.fbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  for (auto& fl : frame_layers) {
    if (!fl.drm->needs_composition()) {
      continue;
    }
    const bool blend = any_composited;
    if (fl.store) {
      CompositeLayerIntoFbo(comp.fbo, fl.store->fbo, fl.store->color_tex,
                            static_cast<GLsizei>(fl.store->width),
                            static_cast<GLsizei>(fl.store->height),
                            static_cast<GLint>(fl.flutter->offset.x),
                            static_cast<GLint>(fl.flutter->offset.y),
                            static_cast<GLsizei>(fl.flutter->size.width),
                            static_cast<GLsizei>(fl.flutter->size.height),
                            blend,
                            /*flip_y=*/true);
      any_composited = true;
    } else if (fl.flutter->type == kFlutterLayerContentTypePlatformView &&
               fl.flutter->platform_view) {
      std::shared_ptr<ICompositorSurface> surface_sp;
      {
        std::lock_guard<std::mutex> lock(surfaces_mu_);
        auto it = surfaces_.find(fl.flutter->platform_view->identifier);
        if (it != surfaces_.end()) {
          surface_sp = it->second;
        }
      }
      if (surface_sp) {
        surface_sp->OnPresent(fl.flutter);
        if (const auto tex = surface_sp->GetGlTextureName(); tex != 0) {
          CompositeLayerIntoFbo(comp.fbo, /*src_fbo=*/0, tex,
                                surface_sp->GetGlTextureWidth(),
                                surface_sp->GetGlTextureHeight(),
                                static_cast<GLint>(fl.flutter->offset.x),
                                static_cast<GLint>(fl.flutter->offset.y),
                                static_cast<GLsizei>(fl.flutter->size.width),
                                static_cast<GLsizei>(fl.flutter->size.height),
                                blend, /*flip_y=*/true);
          any_composited = true;
        }
      }
    }
  }

  glFinish();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // ── Atomic commit ──
  // The test-only apply() above already populated `req` with all plane
  // property assignments. Commit it with the real flags — no need to
  // re-apply.

  if (backend_->cfg_.debug_backend) {
    backend_->flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }

  // Two-phase flags:
  //   First frame: blocking + ALLOW_MODESET, no PAGE_FLIP_EVENT. The
  //     kernel won't deliver a flip-event on a modeset transition on
  //     most drivers — the commit itself returning is our signal.
  //   Steady state: NONBLOCK + PAGE_FLIP_EVENT for vsync-locked flips.
  //     ALLOW_MODESET is not combined with NONBLOCK unless the driver
  //     has opted in via --drm-allow-nonblock-modeset.
  uint32_t commit_flags = 0;
  if (!plane_mode_set_) {
    commit_flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  } else {
    commit_flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
  }

  auto commit_ok = req.commit(commit_flags, this);
  if (!commit_ok) {
    // Latch: stop trying planes for the rest of the session. One WARN
    // per latch so the failure is visible without flooding.
    spdlog::warn(
        "[DrmCompositor] atomic commit failed ({}); latching GL fallback "
        "for remaining session",
        commit_ok.error().message());
    if (backend_->width() != backend_->mode_width() ||
        backend_->height() != backend_->mode_height()) {
      spdlog::error(
          "[DrmCompositor] framed config ({}x{} on {}x{} mode) needs the "
          "atomic plane compositor; legacy fallback can't letterbox. Next "
          "Present will fail.",
          backend_->width(), backend_->height(), backend_->mode_width(),
          backend_->mode_height());
    }
    fallback_latched_ = true;
    return PresentViaGlFallback(layers, layer_count);
  }

  if (!plane_mode_set_) {
    // First commit landed — kernel is scanning out our composition BO.
    // The blocking commit returned only after the modeset completed,
    // so there's no page-flip event in flight.
    plane_mode_set_ = true;
    flip_pending_ = false;

    // Readback + vblank probe to catch the "commit reported success but
    // the pipe isn't actually running" case before Flutter stalls on the
    // next flip wait. Costs one readback and ~2 refresh periods of sleep,
    // once at startup.
    VerifyPipeRunning();

    // Deliver the initial vsync baton now so Flutter can schedule the
    // next frame. Normal PAGE_FLIP_EVENT deliveries take over from here.
    const intptr_t baton =
        backend_->vsync_baton_.exchange(0, std::memory_order_acq_rel);
    if (baton != 0) {
      if (auto engine =
              backend_->vsync_engine_.load(std::memory_order_relaxed)) {
        const uint64_t now = LibFlutterEngine->GetCurrentTime();
        const uint64_t period_ns =
            backend_->vrefresh() > 0
                ? 1000000000ULL / static_cast<uint64_t>(backend_->vrefresh())
                : 16666667ULL;
        LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
      }
    }
  } else {
    flip_pending_ = true;
  }
  comp_idx_ ^= 1;  // swap composition double-buffer for next frame
  output.mark_clean();

  return true;
}

// ─── Backing store create / collect ──────────────────────────────────────

bool DrmCompositor::CreateBackingStore(const FlutterBackingStoreConfig* config,
                                       FlutterBackingStore* out) {
  EnsureGlCapsProbed();

  const auto w = static_cast<uint32_t>(config->size.width);
  const auto h = static_cast<uint32_t>(config->size.height);

  // Match the primary plane's native format so layer 0 can scan out
  // directly without a format conversion. On amdgpu/i915 primary tends
  // to advertise both XR24 and AR24, but some CRTC/mode combinations
  // silently refuse to flip when the new FB format differs from the
  // CRTC's current active format (the TTY's XR24 in our case), leaving
  // the old FB on scanout. Using the resolved primary format avoids it.
  const uint32_t backing_format = backend_->resolved().primary_format;

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
    if (!CreateGbmStore(*store, w, h, backing_format)) {
      return false;
    }
    ++store_pool_misses_;
  }

  auto* baton = new StoreBaton{this, store.get()};

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
    spdlog::debug(
        "[DrmCompositor] CreateBackingStore {}x{} #{} live={} peak={} "
        "pool(hit={} miss={} idle={})",
        w, h, store_create_total_, stores_.size(), store_peak_live_,
        store_pool_hits_, store_pool_misses_, store_pool_.size());
  }
  return true;
}

bool DrmCompositor::CollectBackingStore(const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
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
    spdlog::debug("[DrmCompositor] CollectBackingStore #{} live={} idle={}",
                  store_collect_total_, stores_.size(), store_pool_.size());
  }
  return true;
}

// ─── Surface registry ────────────────────────────────────────────────────

void DrmCompositor::RegisterSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  std::lock_guard lock(surfaces_mu_);
  surfaces_[id] = std::move(surface);
}

void DrmCompositor::UnregisterSurface(FlutterPlatformViewIdentifier id) {
  std::lock_guard lock(surfaces_mu_);
  surfaces_.erase(id);
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
