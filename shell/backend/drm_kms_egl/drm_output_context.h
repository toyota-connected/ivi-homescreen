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

#include <xf86drmMode.h>

namespace homescreen::driver_probe {
struct Resolved;
}

// The per-scanout-output geometry a DrmCompositor targets: one CRTC, its
// connector, and the mode/framebuffer extent. One card's DrmBackend owns the
// shared EGL/GBM/context; each output it drives gets one DrmOutputContext plus
// one DrmCompositor. Values are snapshotted from the resolved DRM state at
// compositor construction. They are fixed for the output's lifetime with one
// exception: a connector reconfigure replaces the whole context through
// DrmCompositor::OnModeChanged, because the mode's timing changed under us
// (#765). Nothing mutates a context in place -- it is rebuilt. The accessor
// names mirror DrmBackend's so the compositor reads its output's geometry here
// instead of from the (single-output) backend; EGL/GBM/device/flip accounting
// still come from the shared backend_.
class DrmOutputContext {
 public:
  DrmOutputContext() = default;
  DrmOutputContext(uint32_t crtc_id,
                   uint32_t crtc_index,
                   uint32_t connector_id,
                   const drmModeModeInfo& mode,
                   uint32_t fb_w,
                   uint32_t fb_h,
                   const homescreen::driver_probe::Resolved* resolved,
                   int rotation = 0,
                   uint32_t render_w = 0,
                   uint32_t render_h = 0)
      : crtc_id_(crtc_id),
        crtc_index_(crtc_index),
        connector_id_(connector_id),
        mode_(mode),
        fb_w_(fb_w),
        fb_h_(fb_h),
        resolved_(resolved),
        rotation_(rotation),
        render_w_(render_w != 0 ? render_w : fb_w),
        render_h_(render_h != 0 ? render_h : fb_h) {}

  [[nodiscard]] uint32_t crtc_id() const { return crtc_id_; }
  [[nodiscard]] uint32_t crtc_index() const { return crtc_index_; }
  [[nodiscard]] uint32_t connector_id() const { return connector_id_; }
  [[nodiscard]] const drmModeModeInfo& mode() const { return mode_; }
  [[nodiscard]] uint32_t mode_width() const { return mode_.hdisplay; }
  [[nodiscard]] uint32_t mode_height() const { return mode_.vdisplay; }
  [[nodiscard]] uint32_t vrefresh() const { return mode_.vrefresh; }
  [[nodiscard]] uint32_t width() const { return fb_w_; }
  [[nodiscard]] uint32_t height() const { return fb_h_; }
  [[nodiscard]] const homescreen::driver_probe::Resolved& resolved() const {
    return *resolved_;
  }
  // --drm-rotation for this output, counter-clockwise in degrees (#848). The
  // picture is turned on the GPU from the render extent, what Flutter lays
  // out at, onto the framebuffer. Unrotated, the two are the same.
  [[nodiscard]] int rotation() const { return rotation_; }
  [[nodiscard]] uint32_t render_width() const { return render_w_; }
  [[nodiscard]] uint32_t render_height() const { return render_h_; }

 private:
  uint32_t crtc_id_ = 0;
  uint32_t crtc_index_ = 0;
  uint32_t connector_id_ = 0;
  drmModeModeInfo mode_{};
  uint32_t fb_w_ = 0;
  uint32_t fb_h_ = 0;
  const homescreen::driver_probe::Resolved* resolved_ = nullptr;
  int rotation_ = 0;
  uint32_t render_w_ = 0;
  uint32_t render_h_ = 0;
};
