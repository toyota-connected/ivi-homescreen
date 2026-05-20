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

#include "backend/drm_kms_egl/drm_cursor.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/cursor/renderer.hpp>
#include <drm-cxx/cursor/theme.hpp>

#include "logging.h"

namespace homescreen {
namespace {

// XCURSOR_SIZE convention is 24 logical px; GNOME/KDE default to 32.
// 24 keeps the sprite small enough that DPI scaling stays in the 64–
// 256 buffer window on every panel we ship.
constexpr uint32_t kBaseLogicalSize = 24;

struct CursorSizing {
  uint32_t sprite{0};
  uint32_t buffer{0};
};

// Per-output HiDPI cursor sizing. Lifted from drm-cxx's
// examples/common/cursor_size.hpp — that header lives under examples/
// and isn't on the public include path.
//
// Sprite = libxcursor target size; buffer = what the kernel cursor
// plane actually scans out (must be at least 64, then snapped to power
// of two for amdgpu DC).
CursorSizing SizingFor(const int fd,
                       const uint32_t connector_id,
                       const drmModeModeInfo& mode,
                       const uint64_t cap_w) {
  uint32_t mm_w = 0;
  if (auto* c = drmModeGetConnector(fd, connector_id); c != nullptr) {
    mm_w = c->mmWidth;
    drmModeFreeConnector(c);
  }

  uint32_t scale = 1;
  if (mm_w != 0 && mode.hdisplay != 0) {
    const double dpi =
        static_cast<double>(mode.hdisplay) * 25.4 / static_cast<double>(mm_w);
    const long stepped = std::lround(dpi / 96.0);
    if (stepped > 1) {
      scale = static_cast<uint32_t>(stepped);
    }
  }

  // Defensive cap: a pathological EDID (mm_w = 1) could otherwise blow
  // scale up and feed a huge target_size to libxcursor.
  constexpr uint32_t kMaxSprite = 256;
  uint32_t sprite = kBaseLogicalSize * scale;
  if (sprite > kMaxSprite) {
    sprite = kMaxSprite;
  }
  uint32_t buffer = 64;
  while (buffer < sprite) {
    buffer *= 2;
  }
  if (cap_w != 0 && buffer > cap_w) {
    buffer = static_cast<uint32_t>(cap_w);
  }
  return {sprite, buffer};
}

const char* PlanePathName(const drm::cursor::PlanePath path) {
  switch (path) {
    case drm::cursor::PlanePath::kAtomicCursor:
      return "atomic-cursor";
    case drm::cursor::PlanePath::kAtomicOverlay:
      return "atomic-overlay";
    case drm::cursor::PlanePath::kLegacy:
      return "legacy";
  }
  return "?";
}

}  // namespace

struct DrmCursor::Impl {
  drm::cursor::Renderer renderer;
  int letterbox_x{0};
  int letterbox_y{0};

  Impl(drm::cursor::Renderer r, int lx, int ly)
      : renderer(std::move(r)), letterbox_x(lx), letterbox_y(ly) {}
};

std::unique_ptr<DrmCursor> DrmCursor::Create(drm::Device& dev,
                                             const uint32_t crtc_id,
                                             const uint32_t connector_id,
                                             const drmModeModeInfo& mode,
                                             const uint32_t fb_w,
                                             const uint32_t fb_h) {
  if (const char* gate = std::getenv("IVI_DRM_CURSOR");
      gate != nullptr && std::string_view(gate) == "0") {
    spdlog::info("[DrmCursor] disabled via IVI_DRM_CURSOR=0");
    return nullptr;
  }

  // Cap. Defaults to 64 if the kernel doesn't report it; that's the
  // legacy floor and works on every driver. Some drivers return 0 from
  // a successful drmGetCap (vkms historically), so coerce that back to
  // 64 too so the buffer clamp doesn't accidentally floor to zero.
  uint64_t cap_w = 64;
  if (drmGetCap(dev.fd(), DRM_CAP_CURSOR_WIDTH, &cap_w) != 0 || cap_w == 0) {
    cap_w = 64;
  }

  const auto sizing = SizingFor(dev.fd(), connector_id, mode, cap_w);

  auto theme = drm::cursor::Theme::discover();
  if (!theme) {
    spdlog::warn("[DrmCursor] no XCursor theme found ({}); no cursor sprite",
                 theme.error().message());
    return nullptr;
  }

  auto cursor = drm::cursor::Cursor::load(*theme, "default", "", sizing.sprite);
  if (!cursor) {
    spdlog::warn("[DrmCursor] load 'default': {}; no cursor sprite",
                 cursor.error().message());
    return nullptr;
  }

  drm::cursor::RendererConfig rcfg;
  rcfg.crtc_id = crtc_id;
  rcfg.preferred_size = sizing.buffer;
  auto renderer = drm::cursor::Renderer::create(dev, rcfg);
  if (!renderer) {
    spdlog::warn("[DrmCursor] renderer create: {}; no cursor sprite",
                 renderer.error().message());
    return nullptr;
  }
  if (auto r = renderer->set_cursor(std::move(*cursor)); !r) {
    spdlog::warn("[DrmCursor] set_cursor: {}; no cursor sprite",
                 r.error().message());
    return nullptr;
  }

  // Do NOT commit move_to here. The CRTC is not ACTIVE yet —
  // SetInitialMode (legacy path) and the compositor's first atomic
  // commit (plane path) both run lazily on the first Present. A
  // cursor commit issued before that would EINVAL on every atomic
  // driver. The first real pointer motion arrives after the first
  // present, so by then the CRTC is live and Move()'s commit lands.

  // Letterbox offset for framed mode (fb smaller than CRTC mode):
  // pointer coordinates from the seat are in fb space; the sprite
  // needs to land at fb-pos + offset in CRTC space.
  const int letterbox_x =
      (static_cast<int>(mode.hdisplay) - static_cast<int>(fb_w)) / 2;
  const int letterbox_y =
      (static_cast<int>(mode.vdisplay) - static_cast<int>(fb_h)) / 2;

  spdlog::info(
      "[DrmCursor] ready (sprite={}px buffer={}px path={} plane_id={} "
      "letterbox={},{})",
      sizing.sprite, sizing.buffer, PlanePathName(renderer->path()),
      renderer->plane_id(), letterbox_x, letterbox_y);

  return std::unique_ptr<DrmCursor>(new DrmCursor(
      std::make_unique<Impl>(std::move(*renderer), letterbox_x, letterbox_y)));
}

DrmCursor::DrmCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DrmCursor::~DrmCursor() = default;

void DrmCursor::Move(const int fb_x, const int fb_y) {
  const int crtc_x = fb_x + impl_->letterbox_x;
  const int crtc_y = fb_y + impl_->letterbox_y;
  if (auto r = impl_->renderer.move_to(crtc_x, crtc_y); !r) {
    spdlog::warn("[DrmCursor] move_to({}, {}): {}", crtc_x, crtc_y,
                 r.error().message());
  }
}

}  // namespace homescreen
