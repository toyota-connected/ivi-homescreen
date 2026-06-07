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

#include <GLES2/gl2.h>

#include "input/cursor_position_sink.h"

namespace homescreen {

// GL-composited mouse cursor for the drm_kms_egl backend — the fallback used
// when the display controller exposes no hardware cursor plane (e.g. i.MX
// LCDIF, which has a single primary plane) or no XCursor theme is available.
// The sprite is a small embedded arrow (premultiplied ARGB8888, no libxcursor
// dependency); it is uploaded once as a GLES2 texture and drawn as a blended
// quad into the window framebuffer (FBO 0) at the end of each Present, on top
// of the rendered Flutter scene.
//
// Threading: SetPosition()/SetVisible() are called from the seat's input
// thread (position + visibility are atomics). EnsureGl()/Draw()/Destroy() run
// on the rasterizer thread with the EGL context current — Draw() from the
// backend's Present() just before eglSwapBuffers, Destroy() from the backend
// teardown while the context is still current.
class GlCursor final : public ICursorPositionSink {
 public:
  GlCursor();
  ~GlCursor() override;

  GlCursor(const GlCursor&) = delete;
  GlCursor& operator=(const GlCursor&) = delete;

  // Input thread. Coordinates are render/viewport pixels; the hotspot (arrow
  // tip) is subtracted in Draw(). Marks the cursor visible on first motion.
  void SetPosition(int32_t x, int32_t y) override {
    x_.store(x, std::memory_order_relaxed);
    y_.store(y, std::memory_order_relaxed);
    visible_.store(true, std::memory_order_release);
  }

  void SetVisible(const bool visible) {
    visible_.store(visible, std::memory_order_release);
  }

  // Rasterizer thread, EGL context current, FBO 0 holding the rendered frame.
  // Composites the cursor over it. No-op until the cursor is visible.
  void Draw(uint32_t fb_w, uint32_t fb_h);

  // Rasterizer thread, EGL context current. Release GL objects.
  void Destroy();

 private:
  bool EnsureGl();

  // Sprite: premultiplied RGBA8888 bytes, row-major, w_ * h_ texels.
  std::vector<uint8_t> rgba_;
  uint32_t w_{0};
  uint32_t h_{0};
  int32_t hot_x_{0};
  int32_t hot_y_{0};
  uint32_t scale_{2};  // integer upscale so a 12x19 arrow is visible at 1080p

  std::atomic<int32_t> x_{0};
  std::atomic<int32_t> y_{0};
  std::atomic<bool> visible_{false};

  GLuint program_{0};
  GLuint texture_{0};
  GLint a_pos_{-1};
  GLint a_uv_{-1};
  GLint u_tex_{-1};
  bool gl_ready_{false};
  bool gl_failed_{false};
};

}  // namespace homescreen
