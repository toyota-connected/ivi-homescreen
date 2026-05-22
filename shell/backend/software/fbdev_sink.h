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
#include <string>

#include "backend/software/surface_sink.h"

// Legacy /dev/fb* framebuffer sink. Useful for embedded SoCs that
// expose a panel via fbdev but lack DRM/KMS, and for CI-host setups
// where the kernel's `vfb` module provides a virtual framebuffer
// without real hardware.
//
// Per-frame path: open + mmap once at Create(), then memcpy +
// byte-swizzle Flutter's RGBA8888 into the framebuffer's native pixel
// layout each Present(). No vsync hook — fbdev's FBIO_WAITFORVSYNC is
// broadly broken across drivers and not exposed here. Pacing comes
// from Flutter's wall-clock scheduler.
//
// Pixel format: 32-bpp BGRA / BGRX is the universal modern layout
// (red.offset=16, green.offset=8, blue.offset=0). FbDevSink refuses
// any other layout (RGB565, palettized, planar) with a clear error so
// the user knows to either reconfigure their console driver or pick a
// different sink.
class FbDevSink final : public ISurfaceSink {
 public:
  // @p device_path is something like "/dev/fb0"; empty defaults to it.
  // Returns nullptr if the device can't be opened, the format isn't
  // supported, or mmap fails.
  static std::unique_ptr<FbDevSink> Create(const std::string& device_path);

  ~FbDevSink() override;

  FbDevSink(const FbDevSink&) = delete;
  FbDevSink& operator=(const FbDevSink&) = delete;

  bool Present(const void* allocation,
               size_t row_bytes,
               size_t height) override;
  // OnSize is intentionally a no-op — the framebuffer geometry is
  // fixed by the panel/driver; the swizzle path clips / pads if
  // Flutter's view geometry differs.

  // For the SoftwareDisplay so it reports the panel's actual size.
  [[nodiscard]] uint32_t fb_width() const { return fb_width_; }
  [[nodiscard]] uint32_t fb_height() const { return fb_height_; }

 private:
  FbDevSink();
  bool Init(const std::string& device_path);

  int fb_fd_{-1};
  uint8_t* fb_map_{nullptr};
  size_t fb_size_{0};
  uint32_t fb_width_{0};
  uint32_t fb_height_{0};
  uint32_t fb_stride_{0};  // bytes per row (line_length)
};
