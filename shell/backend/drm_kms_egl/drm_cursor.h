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

#include <cstdint>
#include <memory>

#include <xf86drmMode.h>

namespace drm {
class Device;
class AtomicRequest;
}  // namespace drm

namespace homescreen {

// KMS hardware cursor wrapping drm::cursor::Renderer.
//
// Wayland compositors give the shell a pointer cursor for free; the
// bare-DRM path doesn't, so a held trackpad / mouse on a head unit
// produces FlutterPointerEvents with no on-screen sprite. DrmCursor
// fixes that by loading the system "default" arrow at startup, picking
// a per-output DPI-scaled size, and committing it on the CRTC's cursor
// plane (atomic on modern drivers, drmModeSetCursor on legacy).
//
// Sized once at creation from the connector's mmWidth + active mode;
// hot-plug rebinding is a future pass. Owned by DrmBackend (which has
// the device, crtc_id, connector_id, and mode at probe time);
// DrmDisplay/DrmSeat are handed a raw pointer for the seat-thread
// HandlePointerMotion path.
//
// Single-thread by ownership: every public method must be called from
// the seat dispatch thread. The compositor's atomic commits race
// harmlessly against the cursor's own atomic commits at the kernel
// boundary (the kernel serializes), but the Renderer itself is not
// thread-safe.
class DrmCursor {
 public:
  // Construct the cursor on the given CRTC for the active mode.
  // `fb_w` / `fb_h` are the framebuffer dimensions the seat clamps the
  // pointer against (== DrmBackend::width()/height()). When framed mode
  // is in effect (fb < mode), Move() adds the letterbox offset so the
  // sprite lands inside the visible content, not in the top-left
  // letterbox region.
  //
  // Loads the system "default" arrow cursor and binds it to a cursor
  // plane (or legacy drmModeSetCursor), but does NOT issue any atomic
  // commit before the first Move() — a commit before the compositor
  // brings the CRTC ACTIVE would EINVAL on every atomic driver.
  //
  // Returns nullptr on any failure (no XCursor theme installed, no
  // suitable plane, drm-cxx built without DRM_CXX_CURSOR, etc.) or
  // when disabled via IVI_DRM_CURSOR=0 — non-fatal: the shell continues
  // without an on-screen cursor.
  // rotation_degrees (0|90|180|270) rotates the sprite to match a rotated
  // scanout. The drm-cxx cursor Renderer drives the plane's rotation property
  // when present, else pre-rotates the pixels + hotspot in software (amdgpu's
  // cursor plane is rotate-0 only, so it takes the software path).
  static std::unique_ptr<DrmCursor> Create(drm::Device& dev,
                                           uint32_t crtc_id,
                                           uint32_t connector_id,
                                           const drmModeModeInfo& mode,
                                           uint32_t fb_w,
                                           uint32_t fb_h,
                                           int rotation_degrees = 0);

  DrmCursor(const DrmCursor&) = delete;
  DrmCursor& operator=(const DrmCursor&) = delete;
  ~DrmCursor();

  // Move the sprite. Coordinates are in framebuffer space (i.e. the
  // same range the seat clamps pointer events to); the letterbox
  // offset captured at Create is applied internally so the sprite
  // lands in the visible content even in framed mode. Issues an
  // atomic commit on the cursor plane (or drmModeMoveCursor on the
  // legacy path). Failures are logged at warn level and otherwise
  // ignored — the pointer remains usable in Flutter even if the
  // sprite stalls.
  void Move(int fb_x, int fb_y);

  // Enable staged mode. In staged mode the cursor plane is driven by the
  // compositor via Stage() instead of self-committing — used on drivers
  // (nvidia-drm) where a separate cursor commit on the CRTC starves the
  // compositor's PAGE_FLIP_EVENT. Must be set before the first
  // SetPosition. The compositor must also be handed this cursor via
  // DrmCompositor::SetCursor.
  void SetStagedMode(bool enabled);

  // Record the desired pointer position in framebuffer coordinates. In
  // staged mode this only stores the position (an atomic, no DRM commit)
  // for the compositor's next Stage() to apply; otherwise it forwards to
  // Move(). Safe to call from the seat dispatch thread.
  void SetPosition(int fb_x, int fb_y);

  // Write the cursor plane state (FB_ID, CRTC, position) into the
  // compositor's AtomicRequest so the cursor rides the same atomic flip.
  // Returns false when there is no pending position yet (cursor not shown)
  // or staging fails. On success, *needs_modeset (when non-null) reports
  // whether the plane's first enable requires DRM_MODE_ATOMIC_ALLOW_MODESET
  // — the caller must then commit that frame blocking. Drives the
  // non-thread-safe drm-cxx Renderer, so call only from the raster
  // (compositor commit) thread.
  [[nodiscard]] bool Stage(drm::AtomicRequest& req, bool* needs_modeset);

  // Self-commit the recorded position. Used by present paths that own
  // their commit and can't accept a staged plane (the USE_DRM_SCENE
  // LayerScene path) so the cursor stays visible in staged mode. No-op
  // outside staged mode or before the first SetPosition. Raster-thread
  // only. NOTE: this is a separate cursor commit on the CRTC and so
  // reintroduces the flip contention on nvidia-drm — a proper fix needs
  // LayerScene to accept the cursor plane in its own commit.
  void CommitPending();

  // The DRM plane id the cursor is scanned out on (0 on the legacy
  // drmModeSetCursor path, which has no addressable plane). On a CRTC
  // with no dedicated cursor plane the renderer takes an overlay; the
  // compositor must reserve that plane so its scene allocator doesn't
  // disable it every commit. Fixed for the cursor's lifetime.
  [[nodiscard]] uint32_t plane_id() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit DrmCursor(std::unique_ptr<Impl> impl);
};

}  // namespace homescreen
