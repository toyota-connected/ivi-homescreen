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
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include <shell/platform/embedder/embedder.h>

/**
 * @brief Result of walking a @c FlutterPlatformView mutation stack.
 *
 * Flutter describes a platform view's transform as an ordered list of
 * mutations (opacity, clip rect, rounded clip, affine transform) applied
 * outer-to-inner. The compositor composes them into a single state that
 * both positions the Wayland subsurface and tells plugin implementations
 * what visual effects they still need to apply themselves.
 *
 * Coordinate convention matches Flutter: row-major 3x3, applied as
 *   w  = g*x + h*y + i
 *   x' = (a*x + c*y + tx) / w
 *   y' = (b*x + d*y + ty) / w
 * where the last row is (0, 0, 1) unless a perspective was composed in.
 *
 * @c clip_rect is in the composed (post-transform) coordinate space: the
 * engine gives each clip in the space it was pushed in, under the transforms
 * outside it, so each is mapped through those before it is intersected.
 */
struct ComposedMutation {
  // Composed transform: the whole 3x3 matrix. @c has_perspective says
  // whether any mutation had a perspective row.
  double a{1.0};
  double b{0.0};
  double c{0.0};
  double d{1.0};
  double tx{0.0};
  double ty{0.0};
  double g{0.0};
  double h{0.0};
  double i{1.0};

  double opacity{1.0};

  // Intersection of all clip_rect mutations, in composed coordinates.
  // Starts as "unbounded" sentinel; @c has_clip flags whether it was
  // ever narrowed. A clip under a transform that turns it by other than a
  // right angle, or under a perspective, is not a rect on screen; its
  // bounding box is used instead and @c clip_exact is cleared.
  FlutterRect clip_rect{
      -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
  bool has_clip{false};
  bool clip_exact{true};

  bool has_rounded_clip{false};
  bool has_perspective{false};

  // The innermost rounded clip that is still a rounded rect on screen, in
  // composed coordinates: the transforms outside it only move, scale or mirror
  // it. Valid when @c has_rounded_mask. Every rounded clip's bounding box is
  // also intersected into @c clip_rect, so a path that cannot round corners
  // still crops to it.
  //
  // Two rounded clips do not intersect into one, and one under a rotation,
  // skew or perspective is not a rounded rect on screen, so in either case
  // @c rounded_clip_exact is cleared: some corners are drawn square.
  FlutterRoundedRect rounded_clip{};
  bool has_rounded_mask{false};
  bool rounded_clip_exact{true};

  /// True when the composed transform is a pure translation + positive
  /// axis-aligned scale. Wayland subsurface + viewport can model this
  /// losslessly; anything else needs a plugin-side shader.
  [[nodiscard]] bool IsAxisAligned() const {
    return std::abs(b) < 1e-6 && std::abs(c) < 1e-6 && a > 0.0 && d > 0.0;
  }

  /// True when opacity, clip shape, or transform type is beyond what the
  /// compositor can apply via Wayland protocol alone. A plugin's
  /// @c ICompositorSurface::OnPresent implementation is responsible.
  [[nodiscard]] bool NeedsPluginComposite() const {
    return opacity < 1.0 - 1e-6 || has_rounded_clip || has_perspective ||
           !IsAxisAligned();
  }
};

/**
 * @brief Utilities for composing @c FlutterPlatformView::mutations.
 */
class MutationStack {
 public:
  /**
   * @brief The bounding box of @p rect under @p m. Under a perspective a
   * corner can reach past the horizon (w not above 0), where the rect has no
   * bounding box; it is then unbounded.
   */
  static FlutterRect MapRect(const ComposedMutation& m,
                             const FlutterRect& rect) {
    const double xs[2] = {rect.left, rect.right};
    const double ys[2] = {rect.top, rect.bottom};
    FlutterRect out{std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max(),
                    -std::numeric_limits<double>::max(),
                    -std::numeric_limits<double>::max()};
    for (const double x : xs) {
      for (const double y : ys) {
        const double w = m.g * x + m.h * y + m.i;
        if (!(w > 1e-9)) {
          return {-std::numeric_limits<double>::max(),
                  -std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max()};
        }
        const double px = (m.a * x + m.c * y + m.tx) / w;
        const double py = (m.b * x + m.d * y + m.ty) / w;
        out.left = std::min(out.left, px);
        out.top = std::min(out.top, py);
        out.right = std::max(out.right, px);
        out.bottom = std::max(out.bottom, py);
      }
    }
    return out;
  }

  /**
   * @brief @p rr under the 2D part of @p m, which must only move, scale or
   * mirror it (b and c zero): its rect mapped, each radius scaled, and the
   * corners swapped across a mirror so each stays the one at that corner on
   * screen.
   */
  static FlutterRoundedRect MapRoundedRect(const ComposedMutation& m,
                                           const FlutterRoundedRect& rr) {
    const double sx = std::abs(m.a);
    const double sy = std::abs(m.d);
    const auto scaled = [sx, sy](const FlutterSize& s) {
      return FlutterSize{s.width * sx, s.height * sy};
    };
    FlutterRoundedRect out{};
    out.rect = MapRect(m, rr.rect);
    out.upper_left_corner_radius = scaled(rr.upper_left_corner_radius);
    out.upper_right_corner_radius = scaled(rr.upper_right_corner_radius);
    out.lower_right_corner_radius = scaled(rr.lower_right_corner_radius);
    out.lower_left_corner_radius = scaled(rr.lower_left_corner_radius);
    if (m.a < 0.0) {
      std::swap(out.upper_left_corner_radius, out.upper_right_corner_radius);
      std::swap(out.lower_left_corner_radius, out.lower_right_corner_radius);
    }
    if (m.d < 0.0) {
      std::swap(out.upper_left_corner_radius, out.lower_left_corner_radius);
      std::swap(out.upper_right_corner_radius, out.lower_right_corner_radius);
    }
    return out;
  }

  /**
   * @brief Intersect @p clip, given in the space under @p r's transform so
   * far, into @p r's clip_rect.
   */
  static void IntersectClip(ComposedMutation& r, const FlutterRect& clip) {
    r.has_clip = true;
    const FlutterRect c = MapRect(r, clip);
    r.clip_rect.left = std::max(r.clip_rect.left, c.left);
    r.clip_rect.top = std::max(r.clip_rect.top, c.top);
    r.clip_rect.right = std::min(r.clip_rect.right, c.right);
    r.clip_rect.bottom = std::min(r.clip_rect.bottom, c.bottom);
    const bool keeps_rect = (std::abs(r.b) < 1e-6 && std::abs(r.c) < 1e-6) ||
                            (std::abs(r.a) < 1e-6 && std::abs(r.d) < 1e-6);
    if (!keeps_rect || r.has_perspective) {
      r.clip_exact = false;
    }
  }

  /**
   * @brief Walk @p view's mutation list front-to-back and compose.
   *
   * Safe with null @p view or zero-length lists: returns an identity
   * result.
   */
  static ComposedMutation Compose(const FlutterPlatformView* view) {
    ComposedMutation r;
    if (!view || view->mutations_count == 0 || !view->mutations) {
      return r;
    }
    for (size_t i = 0; i < view->mutations_count; ++i) {
      const FlutterPlatformViewMutation* m = view->mutations[i];
      if (!m) {
        continue;
      }
      switch (m->type) {
        case kFlutterPlatformViewMutationTypeOpacity:
          r.opacity *= m->opacity;
          break;
        case kFlutterPlatformViewMutationTypeClipRect:
          IntersectClip(r, m->clip_rect);
          break;
        case kFlutterPlatformViewMutationTypeClipRoundedRect: {
          const FlutterRoundedRect& rr = m->clip_rounded_rect;
          IntersectClip(r, rr.rect);
          if (r.has_rounded_clip) {
            r.rounded_clip_exact = false;  // a second one: see rounded_clip
          }
          r.has_rounded_clip = true;
          const bool axis_aligned = std::abs(r.b) < 1e-6 &&
                                    std::abs(r.c) < 1e-6 && !r.has_perspective;
          if (!axis_aligned) {
            r.rounded_clip_exact = false;
            break;
          }
          r.rounded_clip = MapRoundedRect(r, rr);
          r.has_rounded_mask = true;
          break;
        }
        case kFlutterPlatformViewMutationTypeTransformation: {
          const FlutterTransformation& t = m->transformation;
          // The running matrix times this one, all nine entries.
          if (std::abs(t.pers0) > 1e-6 || std::abs(t.pers1) > 1e-6 ||
              std::abs(t.pers2 - 1.0) > 1e-6) {
            r.has_perspective = true;
          }
          const double na = r.a * t.scaleX + r.c * t.skewY + r.tx * t.pers0;
          const double nc = r.a * t.skewX + r.c * t.scaleY + r.tx * t.pers1;
          const double ntx = r.a * t.transX + r.c * t.transY + r.tx * t.pers2;
          const double nb = r.b * t.scaleX + r.d * t.skewY + r.ty * t.pers0;
          const double nd = r.b * t.skewX + r.d * t.scaleY + r.ty * t.pers1;
          const double nty = r.b * t.transX + r.d * t.transY + r.ty * t.pers2;
          const double ng = r.g * t.scaleX + r.h * t.skewY + r.i * t.pers0;
          const double nh = r.g * t.skewX + r.h * t.scaleY + r.i * t.pers1;
          const double ni = r.g * t.transX + r.h * t.transY + r.i * t.pers2;
          r.a = na;
          r.b = nb;
          r.c = nc;
          r.d = nd;
          r.tx = ntx;
          r.ty = nty;
          r.g = ng;
          r.h = nh;
          r.i = ni;
          break;
        }
      }
    }
    return r;
  }
};
