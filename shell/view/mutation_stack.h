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

#include <cmath>
#include <cstddef>
#include <limits>

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
 *   x' = a*x + c*y + tx
 *   y' = b*x + d*y + ty
 *
 * @c clip_rect is in the composed (post-transform) coordinate space.
 */
struct ComposedMutation {
  // Composed affine transform (only the 2D part of FlutterTransformation
  // is retained; pers0/pers1/pers2 are checked for non-identity and
  // reported via @c has_perspective).
  double a{1.0};
  double b{0.0};
  double c{0.0};
  double d{1.0};
  double tx{0.0};
  double ty{0.0};

  double opacity{1.0};

  // Intersection of all clip_rect mutations, in composed coordinates.
  // Starts as "unbounded" sentinel; @c has_clip flags whether it was
  // ever narrowed.
  FlutterRect clip_rect{
      -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
  bool has_clip{false};

  bool has_rounded_clip{false};
  bool has_perspective{false};

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
          r.has_clip = true;
          r.clip_rect.left = std::max(r.clip_rect.left, m->clip_rect.left);
          r.clip_rect.top = std::max(r.clip_rect.top, m->clip_rect.top);
          r.clip_rect.right = std::min(r.clip_rect.right, m->clip_rect.right);
          r.clip_rect.bottom =
              std::min(r.clip_rect.bottom, m->clip_rect.bottom);
          break;
        case kFlutterPlatformViewMutationTypeClipRoundedRect:
          r.has_rounded_clip = true;
          break;
        case kFlutterPlatformViewMutationTypeTransformation: {
          const FlutterTransformation& t = m->transformation;
          // Multiply running affine by this one. Ignore perspective but
          // flag it so callers know the 2D part is an approximation.
          if (std::abs(t.pers0) > 1e-6 || std::abs(t.pers1) > 1e-6 ||
              std::abs(t.pers2 - 1.0) > 1e-6) {
            r.has_perspective = true;
          }
          const double na = r.a * t.scaleX + r.c * t.skewY;
          const double nb = r.b * t.scaleX + r.d * t.skewY;
          const double nc = r.a * t.skewX + r.c * t.scaleY;
          const double nd = r.b * t.skewX + r.d * t.scaleY;
          const double ntx = r.a * t.transX + r.c * t.transY + r.tx;
          const double nty = r.b * t.transX + r.d * t.transY + r.ty;
          r.a = na;
          r.b = nb;
          r.c = nc;
          r.d = nd;
          r.tx = ntx;
          r.ty = nty;
          break;
        }
      }
    }
    return r;
  }
};
