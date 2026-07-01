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
// one DrmCompositor. Values are snapshotted
// from the resolved DRM state at compositor construction and fixed for the
// output's lifetime. The accessor names mirror DrmBackend's so the compositor
// reads its output's geometry here instead of from the (single-output) backend;
// EGL/GBM/device/flip accounting still come from the shared backend_.
class DrmOutputContext {
 public:
  DrmOutputContext() = default;
  DrmOutputContext(uint32_t crtc_id,
                   uint32_t crtc_index,
                   uint32_t connector_id,
                   const drmModeModeInfo& mode,
                   uint32_t fb_w,
                   uint32_t fb_h,
                   const homescreen::driver_probe::Resolved* resolved)
      : crtc_id_(crtc_id),
        crtc_index_(crtc_index),
        connector_id_(connector_id),
        mode_(mode),
        fb_w_(fb_w),
        fb_h_(fb_h),
        resolved_(resolved) {}

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

 private:
  uint32_t crtc_id_ = 0;
  uint32_t crtc_index_ = 0;
  uint32_t connector_id_ = 0;
  drmModeModeInfo mode_{};
  uint32_t fb_w_ = 0;
  uint32_t fb_h_ = 0;
  const homescreen::driver_probe::Resolved* resolved_ = nullptr;
};
