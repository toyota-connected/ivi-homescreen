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
#include <cstdint>
#include <vector>

// Software mouse cursor for the CPU/software backend. The dumb-buffer sinks
// repaint the whole back buffer from Flutter every Present, so the cursor is
// just composited on top after the swizzle and before the page flip — no
// save/restore of the pixels under the cursor is needed.
//
// The bitmap is a small embedded arrow (premultiplied ARGB8888), so the
// software backend keeps its "no GBM / no GL / no extra libraries" property —
// no libxcursor dependency. The hotspot is the arrow tip.
//
// Threading: SetPosition()/SetVisible() are called from the input thread
// (SoftwareSeat); BlendXRGB() is called from the rasterizer thread (the sink's
// Present). Position + visibility are atomics; the bitmap is immutable after
// construction.
class SoftwareCursor {
 public:
  SoftwareCursor();

  // Move the cursor and mark it visible. Coordinates are in framebuffer
  // (mode) pixels; the hotspot is subtracted in BlendXRGB.
  void SetPosition(int32_t x, int32_t y) {
    x_.store(x, std::memory_order_relaxed);
    y_.store(y, std::memory_order_relaxed);
    visible_.store(true, std::memory_order_release);
  }

  void SetVisible(bool visible) {
    visible_.store(visible, std::memory_order_release);
  }

  [[nodiscard]] bool visible() const {
    return visible_.load(std::memory_order_acquire);
  }

  // Composite the cursor into an XRGB8888 (memory order [B,G,R,X]) buffer of
  // @p fb_w x @p fb_h with row stride @p pitch bytes. No-op when hidden. Edge
  // pixels are clipped. Premultiplied "over" blend.
  void BlendXRGB(uint8_t* map,
                 uint32_t fb_w,
                 uint32_t fb_h,
                 uint32_t pitch) const;

 private:
  // Premultiplied ARGB8888 (0xAARRGGBB), row-major, width_ * height_ entries.
  std::vector<uint32_t> pixels_;
  uint32_t width_{0};
  uint32_t height_{0};
  int32_t hot_x_{0};
  int32_t hot_y_{0};

  std::atomic<int32_t> x_{0};
  std::atomic<int32_t> y_{0};
  std::atomic<bool> visible_{false};
};