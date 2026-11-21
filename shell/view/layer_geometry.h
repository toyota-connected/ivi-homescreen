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
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

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

// A rounded rect: a rect and the radii of its corners, each (x, y), clockwise
// from the upper left. Zero radii are square corners.
struct RoundedRect {
  RectF rect;
  std::array<std::array<float, 2>, 4> radii{};  // upper left, upper right,
                                                // lower right, lower left
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

// @p uv for a buffer whose row 0 is the bottom row: the same placement, its
// rows read the other way up. Layer geometry stays in the image's displayed
// orientation; only the memory order differs.
[[nodiscard]] inline UvAffine FlipRows(UvAffine uv) {
  uv.b = -uv.b;
  uv.d = -uv.d;
  uv.ty = 1.0F - uv.ty;
  return uv;
}

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

// Clip a layer to @p bounds, in the same coordinates as @p dst, and crop @p src
// to the part still shown. A zero-size @p src means the whole buffer and is
// made explicit first. Returns false when nothing of the layer is left, in
// which case @p src and @p dst are unchanged.
inline bool ClipLayerToRect(RectF* src,
                            RectI* dst,
                            const uint32_t buffer_w,
                            const uint32_t buffer_h,
                            const BufferTransform transform,
                            const RectI& bounds) {
  if (dst->w <= 0 || dst->h <= 0 || bounds.w <= 0 || bounds.h <= 0) {
    return false;
  }
  const int32_t x0 = std::max(dst->x, bounds.x);
  const int32_t y0 = std::max(dst->y, bounds.y);
  const int32_t x1 = std::min(dst->x + dst->w, bounds.x + bounds.w);
  const int32_t y1 = std::min(dst->y + dst->h, bounds.y + bounds.h);
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

// Clip a layer to the view, which spans [0, view_w) x [0, view_h) in the same
// coordinates as @p dst. See ClipLayerToRect.
inline bool ClipLayerToView(RectF* src,
                            RectI* dst,
                            const uint32_t buffer_w,
                            const uint32_t buffer_h,
                            const BufferTransform transform,
                            const int32_t view_w,
                            const int32_t view_h) {
  return ClipLayerToRect(src, dst, buffer_w, buffer_h, transform,
                         RectI{0, 0, view_w, view_h});
}

// The part of the view a layer may cover, in view-local pixels: the whole view,
// cut down to @p clip (output pixels) when there is one. Empty when the clip
// misses the view.
inline RectI ViewBounds(const RectI& view, const RectI* clip) {
  RectI b{0, 0, view.w, view.h};
  if (clip == nullptr) {
    return b;
  }
  const int32_t x0 = std::max(0, clip->x - view.x);
  const int32_t y0 = std::max(0, clip->y - view.y);
  const int32_t x1 = std::min(view.w, clip->x + clip->w - view.x);
  const int32_t y1 = std::min(view.h, clip->y + clip->h - view.y);
  if (x1 <= x0 || y1 <= y0) {
    return RectI{0, 0, 0, 0};
  }
  return RectI{x0, y0, x1 - x0, y1 - y0};
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
// over a @p buffer_w x @p buffer_h image. The layer is clipped to the view,
// and to @p clip (output pixels) when one is given. Returns false when nothing
// of it is visible.
inline bool PlaceLayer(RectF src,
                       RectI dst,
                       const BufferTransform transform,
                       const bool opaque,
                       const uint32_t buffer_w,
                       const uint32_t buffer_h,
                       const RectI& view,
                       LayerPlacement* out,
                       const RectI* clip = nullptr) {
  if (buffer_w == 0 || buffer_h == 0) {
    return false;
  }
  if (dst.w <= 0 || dst.h <= 0) {
    dst = {0, 0, view.w, view.h};
  }
  if (!ClipLayerToRect(&src, &dst, buffer_w, buffer_h, transform,
                       ViewBounds(view, clip))) {
    return false;
  }
  out->dst = {view.x + dst.x, view.y + dst.y, dst.w, dst.h};
  out->uv = UvForLayer(src, buffer_w, buffer_h, transform);
  out->opaque = opaque;
  return true;
}

// @p uv for the part @p sub of the destination rect @p dst: the same texture
// coordinates at every pixel, with (s, t) running over @p sub instead. An empty
// @p dst has no (s, t) to map, and gives back @p uv.
inline UvAffine SubUv(const UvAffine& uv, const RectI& dst, const RectI& sub) {
  if (dst.w <= 0 || dst.h <= 0) {
    return uv;
  }
  const double s0 = static_cast<double>(sub.x - dst.x) / dst.w;
  const double t0 = static_cast<double>(sub.y - dst.y) / dst.h;
  const double sw = static_cast<double>(sub.w) / dst.w;
  const double th = static_cast<double>(sub.h) / dst.h;
  UvAffine out;
  out.a = static_cast<float>(uv.a * sw);
  out.b = static_cast<float>(uv.b * sw);
  out.c = static_cast<float>(uv.c * th);
  out.d = static_cast<float>(uv.d * th);
  out.tx = uv.U(s0, t0);
  out.ty = uv.V(s0, t0);
  return out;
}

// A part of a layer's destination rect, and whether a corner of its rounded
// clip may cut it.
struct RoundedPiece {
  RectI rect;
  bool corner{false};
};

// Split @p dst into the four boxes around @p clip's corners, rounded out to
// whole pixels, and the rest, which no corner reaches. Only the corners need
// the mask; the rest draws like a layer without one. Writes up to seven pieces,
// none empty, that cover @p dst exactly, and returns how many. Where two
// corners' boxes meet, the corners take the whole row or column.
inline size_t SplitRoundedCorners(const RectI& dst,
                                  const RoundedRect& clip,
                                  std::array<RoundedPiece, 7>& out) {
  // Upper left, upper right, lower right, lower left.
  const auto& r = clip.radii;
  const auto split = [](const double lo_edge, const double hi_edge,
                        const int32_t lo, const int32_t hi) {
    auto a = static_cast<int32_t>(std::clamp(
        std::ceil(lo_edge), static_cast<double>(lo), static_cast<double>(hi)));
    auto b = static_cast<int32_t>(std::clamp(
        std::floor(hi_edge), static_cast<double>(lo), static_cast<double>(hi)));
    return std::array<int32_t, 4>{lo, a, std::max(a, b), hi};
  };
  const std::array<int32_t, 4> xs =
      split(clip.rect.x + std::max(r[0][0], r[3][0]),
            clip.rect.x + clip.rect.w - std::max(r[1][0], r[2][0]), dst.x,
            dst.x + dst.w);
  const std::array<int32_t, 4> ys =
      split(clip.rect.y + std::max(r[0][1], r[1][1]),
            clip.rect.y + clip.rect.h - std::max(r[3][1], r[2][1]), dst.y,
            dst.y + dst.h);
  size_t n = 0;
  const auto add = [&](const int32_t x0, const int32_t y0, const int32_t x1,
                       const int32_t y1, const bool corner) {
    if (x1 > x0 && y1 > y0) {
      out[n++] = RoundedPiece{RectI{x0, y0, x1 - x0, y1 - y0}, corner};
    }
  };
  for (const auto& [y0, y1] :
       {std::pair{ys[0], ys[1]}, std::pair{ys[2], ys[3]}}) {
    add(xs[0], y0, xs[1], y1, true);
    add(xs[1], y0, xs[2], y1, false);
    add(xs[2], y0, xs[3], y1, true);
  }
  add(xs[0], ys[1], xs[3], ys[2], false);
  return n;
}

// A map of a view's own pixels to output pixels, projective in general:
//
//   w  = g * x + h * y + i
//   x' = (a * x + c * y + tx) / w
//   y' = (b * x + d * y + ty) / w
//
// For a view under a rotation, skew, mirror or perspective, which no rect on
// the output can show. Without a perspective the last row is (0, 0, 1) and
// the map is affine.
struct ViewMap {
  double a{1};
  double b{0};
  double c{0};
  double d{1};
  double tx{0};
  double ty{0};
  double g{0};
  double h{0};
  double i{1};

  // w at (x, y): above 0 where the point is in front of the viewer.
  [[nodiscard]] double W(const double x, const double y) const {
    return g * x + h * y + i;
  }
  // The output point for (x, y). Meaningful only where W is above 0.
  [[nodiscard]] std::array<double, 2> Map(const double x,
                                          const double y) const {
    const double w = W(x, y);
    return {(a * x + c * y + tx) / w, (b * x + d * y + ty) / w};
  }
};

// Where one layer of a transformed view lands: a quad on the output, in
// homogeneous output pixels. Its corner at (s, t) = (0, 0) and its edges
// along s and t give the point times w, and w0, ws and wt give w, both
// affine in (s, t); the point itself is the one divided by the other. Without
// a perspective w is 1 and the quad a parallelogram. (s, t) run over the layer
// as for LayerPlacement, so @c uv maps them to texture coordinates the same
// way; a draw that divides by w per pixel, as a GPU's perspective-correct
// interpolation does, samples each pixel right. @c bounds holds the quad,
// grown as @c grow says, rounded out to whole pixels and cut down to the clip;
// a draw clipped to it shows what the clip lets through.
//
// The quad's edges are drawn smooth where they are the view's own edges: a
// pixel the edge crosses is covered in part, and is drawn with that share of
// its color. @c grow says how far past each such edge the quad is drawn, in
// units of s or t (s = 0, s = 1, t = 0, t = 1), far enough that the pixels it
// crosses are all drawn; 0 is an edge left hard. Only the view's own edges
// are smoothed: where two layers of a view meet, both smoothed would let what
// is under them show through the seam.
struct QuadPlacement {
  std::array<double, 2> origin{};
  std::array<double, 2> edge_s{};
  std::array<double, 2> edge_t{};
  double w0{1};
  double ws{0};
  double wt{0};
  UvAffine uv;
  RectI bounds;
  bool opaque{false};
  std::array<double, 4> grow{};

  // Whether any edge is smoothed, which leaves the layer translucent there.
  [[nodiscard]] bool Smoothed() const {
    return grow[0] > 0 || grow[1] > 0 || grow[2] > 0 || grow[3] > 0;
  }

  // w at (s, t), above 0 over the whole quad.
  [[nodiscard]] double W(const double s, const double t) const {
    return w0 + s * ws + t * wt;
  }
  // The output point at (s, t).
  [[nodiscard]] std::array<double, 2> At(const double s, const double t) const {
    const double w = W(s, t);
    return {(origin[0] + s * edge_s[0] + t * edge_t[0]) / w,
            (origin[1] + s * edge_s[1] + t * edge_t[1]) / w};
  }
  // The corners of the quad as drawn, grown past its smoothed edges:
  // (s0, t0), (s1, t0), (s0, t1), (s1, t1) in (s, t).
  [[nodiscard]] std::array<std::array<double, 2>, 4> GrownCorners() const {
    const double s0 = -grow[0];
    const double s1 = 1.0 + grow[1];
    const double t0 = -grow[2];
    const double t1 = 1.0 + grow[3];
    return {{{s0, t0}, {s1, t0}, {s0, t1}, {s1, t1}}};
  }
};

// Where (s, t) is held when the layer is sampled: (s0, t0, s1, t1). Half a
// texel inside a smoothed edge, so that a pixel drawn past it, or sampled at
// it, filters only the layer's own texels and not the buffer beyond its crop;
// 0 and 1 at an edge left hard, where the next layer takes over. @p q.uv must
// be the one sampled with, and @p buffer_w x @p buffer_h the buffer's size.
inline std::array<double, 4> SampleClamp(const QuadPlacement& q,
                                         const uint32_t buffer_w,
                                         const uint32_t buffer_h) {
  // Texels along s and along t.
  const double ns = std::abs(q.uv.a) * buffer_w + std::abs(q.uv.b) * buffer_h;
  const double nt = std::abs(q.uv.c) * buffer_w + std::abs(q.uv.d) * buffer_h;
  const double hs = ns > 1.0 ? 0.5 / ns : 0.5;
  const double ht = nt > 1.0 ? 0.5 / nt : 0.5;
  return {q.grow[0] > 0 ? hs : 0.0, q.grow[2] > 0 ? ht : 0.0,
          q.grow[1] > 0 ? 1.0 - hs : 1.0, q.grow[3] > 0 ? 1.0 - ht : 1.0};
}

// The distance in output pixels from (x, y) to each edge of @p q's quad, the
// lines through its corners (s = 0, s = 1, t = 0, t = 1), above 0 on the
// quad's side. The quad's edges are straight on the output under a
// perspective as well, so each distance is affine in (x, y): worked out at the
// corners of a draw, it can be interpolated linearly over the screen. An edge
// of no length gives 0.
inline std::array<double, 4> EdgeDistances(const QuadPlacement& q,
                                           const double x,
                                           const double y) {
  const auto c00 = q.At(0, 0);
  const auto c10 = q.At(1, 0);
  const auto c01 = q.At(0, 1);
  const auto c11 = q.At(1, 1);
  // The line through a and b, signed so that @p inside, a point of the quad
  // off it, is above 0.
  const auto dist = [x, y](const std::array<double, 2>& a,
                           const std::array<double, 2>& b,
                           const std::array<double, 2>& inside) {
    const double nx = a[1] - b[1];
    const double ny = b[0] - a[0];
    const double len = std::hypot(nx, ny);
    if (!(len > 1e-12)) {
      return 0.0;
    }
    const double side = nx * (inside[0] - a[0]) + ny * (inside[1] - a[1]);
    const double sign = side < 0 ? -1.0 : 1.0;
    return sign * (nx * (x - a[0]) + ny * (y - a[1])) / len;
  };
  // The quad's center is inside it whatever the corners' order.
  const auto mid = q.At(0.5, 0.5);
  return {dist(c00, c01, mid), dist(c10, c11, mid), dist(c00, c10, mid),
          dist(c01, c11, mid)};
}

// How far past each of the quad's edges flagged in @p edges (bit 0 s = 0,
// bit 1 s = 1, bit 2 t = 0, bit 3 t = 1) it must be drawn for the edge to
// move out by @p px output pixels, in units of s or t (QuadPlacement::grow).
// Taken at both ends of the edge, which under a perspective move out by
// different amounts, and the larger kept. Held to the size of the quad, and
// to where w stays above half its value at the edge, so a grown corner never
// reaches past the horizon.
inline std::array<double, 4> GrowForEdges(const QuadPlacement& q,
                                          const unsigned edges,
                                          const double px) {
  std::array<double, 4> grow{};
  // How far one unit of s (along_s) or t moves the point at (s, t) off the
  // edge through it along the other, in output pixels.
  const auto rate = [&q](const double s, const double t, const bool along_s) {
    const double w = q.W(s, t);
    const auto p = q.At(s, t);
    const std::array<double, 2> ds = {(q.edge_s[0] - p[0] * q.ws) / w,
                                      (q.edge_s[1] - p[1] * q.ws) / w};
    const std::array<double, 2> dt = {(q.edge_t[0] - p[0] * q.wt) / w,
                                      (q.edge_t[1] - p[1] * q.wt) / w};
    const auto& move = along_s ? ds : dt;
    const auto& edge = along_s ? dt : ds;
    const double len = std::hypot(edge[0], edge[1]);
    if (!(len > 1e-12)) {
      return 0.0;
    }
    return std::abs(move[0] * edge[1] - move[1] * edge[0]) / len;
  };
  for (unsigned e = 0; e < 4; ++e) {
    if ((edges & (1U << e)) == 0) {
      continue;
    }
    const bool along_s = e < 2;
    const double at = (e % 2 == 0) ? 0.0 : 1.0;
    double g = 0;
    double w_room = std::numeric_limits<double>::infinity();
    for (const double other : {0.0, 1.0}) {
      const double s = along_s ? at : other;
      const double t = along_s ? other : at;
      const double r = rate(s, t, along_s);
      g = std::max(g, r > 1e-12 ? px / r : 1.0);
      // w changes by -dw per unit outward; keep it above half.
      const double dw = (e % 2 == 0 ? 1.0 : -1.0) * (along_s ? q.ws : q.wt);
      if (dw > 0) {
        w_room = std::min(w_room, 0.5 * q.W(s, t) / dw);
      }
    }
    grow[e] = std::min({g, 1.0, w_room});
  }
  // Grown on both sides a corner meets, its w falls by both; keep the
  // corners in front of the viewer, giving up the smoothing if they cannot be.
  for (int tries = 0; tries < 8; ++tries) {
    bool ok = true;
    for (const auto& [s, t] :
         {std::pair{-grow[0], -grow[2]}, std::pair{1.0 + grow[1], -grow[2]},
          std::pair{-grow[0], 1.0 + grow[3]},
          std::pair{1.0 + grow[1], 1.0 + grow[3]}}) {
      ok = ok && q.W(s, t) > 1e-9;
    }
    if (ok) {
      return grow;
    }
    for (double& g : grow) {
      g *= 0.5;
    }
  }
  return {};
}

// Place a layer of a view that spans [0, view_w) x [0, view_h) in its own
// pixels and reaches the output through @p map. @p src and @p dst are the
// layer's geometry as for PlaceLayer, @p dst in the view's own pixels. The
// layer is cropped to the view there, and its bounds are cut down to @p clip
// (output pixels) when one is given. Returns false when nothing of it can show,
// or when part of it is past the horizon of a perspective.
inline bool PlaceLayerThrough(RectF src,
                              RectI dst,
                              const BufferTransform transform,
                              const bool opaque,
                              const uint32_t buffer_w,
                              const uint32_t buffer_h,
                              const int32_t view_w,
                              const int32_t view_h,
                              const ViewMap& map,
                              QuadPlacement* out,
                              const RectI* clip = nullptr) {
  if (buffer_w == 0 || buffer_h == 0 || view_w <= 0 || view_h <= 0) {
    return false;
  }
  if (dst.w <= 0 || dst.h <= 0) {
    dst = {0, 0, view_w, view_h};
  }
  if (!ClipLayerToView(&src, &dst, buffer_w, buffer_h, transform, view_w,
                       view_h)) {
    return false;
  }
  QuadPlacement q;
  q.origin = {map.a * dst.x + map.c * dst.y + map.tx,
              map.b * dst.x + map.d * dst.y + map.ty};
  q.edge_s = {map.a * dst.w, map.b * dst.w};
  q.edge_t = {map.c * dst.h, map.d * dst.h};
  q.w0 = map.W(dst.x, dst.y);
  q.ws = map.g * dst.w;
  q.wt = map.h * dst.h;
  // w is affine in (s, t), so above 0 at the four corners is above 0
  // everywhere between. A layer that reaches past the horizon has no picture
  // there to draw.
  for (const auto& [s, t] : {std::pair{0.0, 0.0}, std::pair{1.0, 0.0},
                             std::pair{0.0, 1.0}, std::pair{1.0, 1.0}}) {
    if (!(q.W(s, t) > 1e-9)) {
      return false;
    }
  }
  // The edges of the layer that are the view's own are smoothed, grown by a
  // pixel: the pixels they cross reach half a pixel past them.
  unsigned edges = 0;
  edges |= dst.x <= 0 ? 1U : 0U;
  edges |= dst.x + dst.w >= view_w ? 2U : 0U;
  edges |= dst.y <= 0 ? 4U : 0U;
  edges |= dst.y + dst.h >= view_h ? 8U : 0U;
  q.grow = GrowForEdges(q, edges, 1.0);
  const auto corners = q.GrownCorners();
  const auto o = q.At(corners[0][0], corners[0][1]);
  double x0 = o[0];
  double y0 = o[1];
  double x1 = x0;
  double y1 = y0;
  for (size_t k = 1; k < corners.size(); ++k) {
    const auto p = q.At(corners[k][0], corners[k][1]);
    x0 = std::min(x0, p[0]);
    y0 = std::min(y0, p[1]);
    x1 = std::max(x1, p[0]);
    y1 = std::max(y1, p[1]);
  }
  if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
      !std::isfinite(y1)) {
    return false;
  }
  // Clamped well inside int32_t before the cast; an output is far smaller.
  constexpr double kLimit = 1 << 24;
  const auto lo = [&](const double v) {
    return static_cast<int32_t>(std::clamp(std::floor(v), -kLimit, kLimit));
  };
  const auto hi = [&](const double v) {
    return static_cast<int32_t>(std::clamp(std::ceil(v), -kLimit, kLimit));
  };
  int32_t bx0 = lo(x0);
  int32_t by0 = lo(y0);
  int32_t bx1 = hi(x1);
  int32_t by1 = hi(y1);
  if (clip != nullptr) {
    bx0 = std::max(bx0, clip->x);
    by0 = std::max(by0, clip->y);
    bx1 = std::min(bx1, clip->x + clip->w);
    by1 = std::min(by1, clip->y + clip->h);
  }
  if (bx1 <= bx0 || by1 <= by0) {
    return false;
  }
  q.bounds = {bx0, by0, bx1 - bx0, by1 - by0};
  q.uv = UvForLayer(src, buffer_w, buffer_h, transform);
  q.opaque = opaque;
  *out = q;
  return true;
}
