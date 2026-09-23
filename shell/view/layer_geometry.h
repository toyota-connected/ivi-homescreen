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

#include <algorithm>
#include <cstdint>

// Where a layer's pixels come from and where they land: a crop of its buffer,
// a transform, and a destination rect. The same description drives the GL and
// Vulkan composite paths, so each gets it from here rather than deriving its
// own texture coordinates.

// The eight buffer transforms. Same values and meaning as
// wl_output.transform: the transform the producer already applied to its
// buffer, which the compositor undoes when it draws. NORMAL is the identity.
enum class BufferTransform : uint32_t {
  kNormal = 0,
  k90 = 1,
  k180 = 2,
  k270 = 3,
  kFlipped = 4,
  kFlipped90 = 5,
  kFlipped180 = 6,
  kFlipped270 = 7,
};

// A rect in buffer pixels, which may be fractional (a wp_viewporter source
// rect is in 1/256ths of a pixel).
struct RectF {
  double x{0};
  double y{0};
  double w{0};
  double h{0};
};

// A rect in whole destination pixels, top-left origin.
struct RectI {
  int32_t x{0};
  int32_t y{0};
  int32_t w{0};
  int32_t h{0};
};

// Normalized texture coordinates for a point of the destination rect:
//
//   u = a * s + c * t + tx
//   v = b * s + d * t + ty
//
// where (s, t) run over [0, 1] across the destination rect with (0, 0) at its
// top-left, and (u, v) run over [0, 1] across the whole buffer with v = 0 on
// the buffer's first row in memory.
struct UvAffine {
  float a{1};
  float b{0};
  float c{0};
  float d{1};
  float tx{0};
  float ty{0};

  // The texture coordinates of destination point (s, t).
  [[nodiscard]] float U(double s, double t) const {
    return static_cast<float>(a * s + c * t + tx);
  }
  [[nodiscard]] float V(double s, double t) const {
    return static_cast<float>(b * s + d * t + ty);
  }
};

// Where destination point (s, t), normalized over the destination rect, lands
// in the buffer, normalized over the source crop. wl_output.transform names
// what the producer did (rotate counter-clockwise; FLIPPED mirrors around the
// vertical axis first), so this is its inverse: at 90 degrees the top-left of
// the destination shows the bottom-left of the buffer. Weston's
// weston_transformed_coord, normalized.
inline void TransformedCoord(const BufferTransform transform,
                             const double s,
                             const double t,
                             double* u,
                             double* v) {
  switch (transform) {
    case BufferTransform::kNormal:
      *u = s;
      *v = t;
      return;
    case BufferTransform::k90:
      *u = t;
      *v = 1 - s;
      return;
    case BufferTransform::k180:
      *u = 1 - s;
      *v = 1 - t;
      return;
    case BufferTransform::k270:
      *u = 1 - t;
      *v = s;
      return;
    case BufferTransform::kFlipped:
      *u = 1 - s;
      *v = t;
      return;
    case BufferTransform::kFlipped90:
      *u = t;
      *v = s;
      return;
    case BufferTransform::kFlipped180:
      *u = s;
      *v = 1 - t;
      return;
    case BufferTransform::kFlipped270:
      *u = 1 - t;
      *v = 1 - s;
      return;
  }
  *u = s;  // out-of-range value: draw untransformed rather than garbage
  *v = t;
}

// The texture-coordinate affine that draws @p src (a crop of a
// @p buffer_w x @p buffer_h buffer, in its own pixels) through @p transform.
// A zero-size @p src means the whole buffer.
inline UvAffine UvForLayer(RectF src,
                           const uint32_t buffer_w,
                           const uint32_t buffer_h,
                           const BufferTransform transform) {
  if (src.w <= 0 || src.h <= 0) {
    src = {0, 0, static_cast<double>(buffer_w), static_cast<double>(buffer_h)};
  }
  const double bw = buffer_w > 0 ? buffer_w : 1;
  const double bh = buffer_h > 0 ? buffer_h : 1;
  // The map is affine, so three points fix it: the destination's origin and
  // its two unit steps.
  double u0 = 0, v0 = 0, us = 0, vs = 0, ut = 0, vt = 0;
  TransformedCoord(transform, 0, 0, &u0, &v0);
  TransformedCoord(transform, 1, 0, &us, &vs);
  TransformedCoord(transform, 0, 1, &ut, &vt);
  const auto nu = [&](double u) { return (src.x + u * src.w) / bw; };
  const auto nv = [&](double v) { return (src.y + v * src.h) / bh; };
  UvAffine m;
  m.tx = static_cast<float>(nu(u0));
  m.ty = static_cast<float>(nv(v0));
  m.a = static_cast<float>(nu(us) - nu(u0));
  m.b = static_cast<float>(nv(vs) - nv(v0));
  m.c = static_cast<float>(nu(ut) - nu(u0));
  m.d = static_cast<float>(nv(vt) - nv(v0));
  return m;
}

// The destination rect's size in buffer terms: a 90- or 270-degree transform
// swaps width and height.
inline bool TransformSwapsAxes(const BufferTransform transform) {
  switch (transform) {
    case BufferTransform::k90:
    case BufferTransform::k270:
    case BufferTransform::kFlipped90:
    case BufferTransform::kFlipped270:
      return true;
    default:
      return false;
  }
}

// Clip a layer to the view, which spans [0, view_w) x [0, view_h) in the same
// coordinates as @p dst, and crop @p src to the part still shown. A zero-size
// @p src means the whole buffer and is made explicit first. Returns false when
// nothing of the layer is left, in which case @p src and @p dst are unchanged.
inline bool ClipLayerToView(RectF* src,
                            RectI* dst,
                            const uint32_t buffer_w,
                            const uint32_t buffer_h,
                            const BufferTransform transform,
                            const int32_t view_w,
                            const int32_t view_h) {
  if (dst->w <= 0 || dst->h <= 0 || view_w <= 0 || view_h <= 0) {
    return false;
  }
  const int32_t x0 = std::max(dst->x, 0);
  const int32_t y0 = std::max(dst->y, 0);
  const int32_t x1 = std::min(dst->x + dst->w, view_w);
  const int32_t y1 = std::min(dst->y + dst->h, view_h);
  if (x1 <= x0 || y1 <= y0) {
    return false;
  }
  RectF s = *src;
  if (s.w <= 0 || s.h <= 0) {
    s = {0, 0, static_cast<double>(buffer_w), static_cast<double>(buffer_h)};
  }
  // The kept part of the destination, normalized over the original rect.
  const double s0 = static_cast<double>(x0 - dst->x) / dst->w;
  const double s1 = static_cast<double>(x1 - dst->x) / dst->w;
  const double t0 = static_cast<double>(y0 - dst->y) / dst->h;
  const double t1 = static_cast<double>(y1 - dst->y) / dst->h;
  // Its corners in the source crop. The transforms are right-angle rotations
  // and reflections, so the image of an axis-aligned rect is axis-aligned and
  // two opposite corners bound it.
  double ua = 0, va = 0, ub = 0, vb = 0;
  TransformedCoord(transform, s0, t0, &ua, &va);
  TransformedCoord(transform, s1, t1, &ub, &vb);
  const double umin = std::min(ua, ub);
  const double umax = std::max(ua, ub);
  const double vmin = std::min(va, vb);
  const double vmax = std::max(va, vb);
  *src = {s.x + umin * s.w, s.y + vmin * s.h, (umax - umin) * s.w,
          (vmax - vmin) * s.h};
  *dst = {x0, y0, x1 - x0, y1 - y0};
  return true;
}

// Where and how to draw one layer of a view on an output.
struct LayerPlacement {
  RectI dst;  // output pixels, top-left origin
  UvAffine uv;
  bool opaque{false};
};

// Place a layer of a view whose rect on the output is @p view (top-left
// origin, output pixels). @p src and @p dst are the layer's own geometry --
// @p dst in view-local pixels, and a zero-size @p dst meaning the whole view --
// over a @p buffer_w x @p buffer_h image. The layer is clipped to the view.
// Returns false when nothing of it is visible.
inline bool PlaceLayer(RectF src,
                       RectI dst,
                       const BufferTransform transform,
                       const bool opaque,
                       const uint32_t buffer_w,
                       const uint32_t buffer_h,
                       const RectI& view,
                       LayerPlacement* out) {
  if (buffer_w == 0 || buffer_h == 0) {
    return false;
  }
  if (dst.w <= 0 || dst.h <= 0) {
    dst = {0, 0, view.w, view.h};
  }
  if (!ClipLayerToView(&src, &dst, buffer_w, buffer_h, transform, view.w,
                       view.h)) {
    return false;
  }
  out->dst = {view.x + dst.x, view.y + dst.y, dst.w, dst.h};
  out->uv = UvForLayer(src, buffer_w, buffer_h, transform);
  out->opaque = opaque;
  return true;
}
