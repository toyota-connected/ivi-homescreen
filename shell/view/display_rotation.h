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

#include "layer_geometry.h"

// A display rotated on the GPU, for a panel whose planes cannot rotate (#848).
//
// Flutter renders upright at the render extent. The panel shows that picture
// turned counter-clockwise by `degrees`, the way a plane's DRM_MODE_ROTATE_*
// turns it, and centered when it is smaller than the panel. Same convention as
// a plane rotation, so a config means the same thing on either path, and the
// DRM seat's pointer and touch transforms hold for both.
//
// Everything here is in pixels with a top-left origin, in render space on the
// way in and panel space on the way out.
struct DisplayRotation {
  int degrees{0};
  int32_t render_w{0};
  int32_t render_h{0};
  int32_t panel_w{0};
  int32_t panel_h{0};

  [[nodiscard]] bool swaps_axes() const {
    return degrees == 90 || degrees == 270;
  }
  // The rotated picture's extent on the panel.
  [[nodiscard]] int32_t rotated_w() const {
    return swaps_axes() ? render_h : render_w;
  }
  [[nodiscard]] int32_t rotated_h() const {
    return swaps_axes() ? render_w : render_h;
  }
  // Where the rotated picture starts on the panel. Zero when it fills it.
  [[nodiscard]] int32_t offset_x() const { return (panel_w - rotated_w()) / 2; }
  [[nodiscard]] int32_t offset_y() const { return (panel_h - rotated_h()) / 2; }

  // Render point (x, y) on the panel. Points, not pixels: pixel (x, y) covers
  // [x, x + 1) x [y, y + 1), so at 90 degrees it lands on panel pixel
  // (y, render_w - 1 - x), which is where the seat places the HW cursor.
  void ToPanel(const double x, const double y, double* px, double* py) const {
    const auto rw = static_cast<double>(render_w);
    const auto rh = static_cast<double>(render_h);
    switch (degrees) {
      case 90:
        *px = y;
        *py = rw - x;
        break;
      case 180:
        *px = rw - x;
        *py = rh - y;
        break;
      case 270:
        *px = rh - y;
        *py = x;
        break;
      default:
        *px = x;
        *py = y;
        break;
    }
    *px += offset_x();
    *py += offset_y();
  }

  // A render rect on the panel. Axis-aligned either way: the turns are all
  // quarter turns.
  [[nodiscard]] RectI ToPanel(const RectI& r) const {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
    ToPanel(r.x, r.y, &x0, &y0);
    ToPanel(static_cast<double>(r.x) + r.w, static_cast<double>(r.y) + r.h, &x1,
            &y1);
    const auto lo = [](double a, double b) { return a < b ? a : b; };
    RectI out;
    out.x = static_cast<int32_t>(lo(x0, x1));
    out.y = static_cast<int32_t>(lo(y0, y1));
    out.w = swaps_axes() ? r.h : r.w;
    out.h = swaps_axes() ? r.w : r.h;
    return out;
  }

  // @p uv draws a texture across a render rect; the result draws it across
  // that rect's panel rect (ToPanel) so the picture turns with the display.
  // (s', t') over the panel rect is (s, t) over the render rect, turned back:
  // at 90 degrees the panel rect's top-left is the render rect's top-right.
  [[nodiscard]] UvAffine ToPanel(const UvAffine& uv) const {
    UvAffine out = uv;
    switch (degrees) {
      case 90:  // (s, t) = (1 - t', s')
        out.a = uv.c;
        out.c = -uv.a;
        out.tx = uv.a + uv.tx;
        out.b = uv.d;
        out.d = -uv.b;
        out.ty = uv.b + uv.ty;
        break;
      case 180:  // (s, t) = (1 - s', 1 - t')
        out.a = -uv.a;
        out.c = -uv.c;
        out.tx = uv.a + uv.c + uv.tx;
        out.b = -uv.b;
        out.d = -uv.d;
        out.ty = uv.b + uv.d + uv.ty;
        break;
      case 270:  // (s, t) = (t', 1 - s')
        out.a = -uv.c;
        out.c = uv.a;
        out.tx = uv.c + uv.tx;
        out.b = -uv.d;
        out.d = uv.b;
        out.ty = uv.d + uv.ty;
        break;
      default:
        break;
    }
    return out;
  }
};
