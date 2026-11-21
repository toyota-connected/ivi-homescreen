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
#include <optional>
#include <utility>
#include <vector>

#include <shell/platform/embedder/embedder.h>

#include "view/layer_geometry.h"
#include "view/mutation_stack.h"

/**
 * @brief Where a platform view is drawn, from its layer and mutations.
 *
 * The engine gives a platform view layer the origin of its transformed
 * bounds, but the size it was laid out at: @c sizePoints times the device
 * pixel ratio, with none of the view's transforms applied (see
 * @c EmbedderLayers::PushPlatformViewLayer). A view under a scale is placed
 * at the right origin and drawn at its unscaled size.
 *
 * The mutation list carries the whole transform, the device pixel ratio
 * included: the framework's root layer scales by it. So the scale the app
 * applied is the composed scale over the pixel ratio, and the drawn size is
 * the layer size times that.
 *
 * A positive axis-aligned scale is corrected in the layer (@c ScaleLayer).
 * A rotation, skew, mirror or perspective cannot be shown as a rect at all,
 * and is drawn through the map @c TransformedViewOf gives.
 */
namespace ihs::platform_view {

/// The scale the app applied to a view, over the device pixel ratio.
struct AppScale {
  double x{1.0};
  double y{1.0};

  [[nodiscard]] bool IsIdentity() const {
    return std::abs(x - 1.0) < 1e-6 && std::abs(y - 1.0) < 1e-6;
  }
};

/**
 * @brief The scale the app applied to @p layer's view.
 *
 * Identity when the layer is not a platform view, has no mutations, the
 * pixel ratio is unknown (zero or less), or the transform is not one a
 * scaled rect can express.
 */
inline AppScale AppScaleOf(const FlutterLayer& layer,
                           const double pixel_ratio) {
  if (layer.type != kFlutterLayerContentTypePlatformView ||
      layer.platform_view == nullptr ||
      layer.platform_view->mutations_count == 0 ||
      !std::isfinite(pixel_ratio) || pixel_ratio <= 0.0) {
    return {};
  }
  const ComposedMutation composed = MutationStack::Compose(layer.platform_view);
  if (composed.has_perspective || !composed.IsAxisAligned()) {
    return {};
  }
  const AppScale s{composed.a / pixel_ratio, composed.d / pixel_ratio};
  if (!std::isfinite(s.x) || !std::isfinite(s.y) || s.x <= 0.0 || s.y <= 0.0) {
    return {};
  }
  return s;
}

/**
 * @brief Set @p out to @p layer with its size scaled to the drawn size.
 *
 * Returns false, and leaves @p out alone, when the view is not scaled. The
 * offset is kept: for a positive axis-aligned transform the engine's offset
 * is already the drawn origin.
 */
inline bool ScaleLayer(const FlutterLayer& layer,
                       const double pixel_ratio,
                       FlutterLayer* out) {
  const AppScale s = AppScaleOf(layer, pixel_ratio);
  if (s.IsIdentity() || out == nullptr) {
    return false;
  }
  *out = layer;
  out->size.width = layer.size.width * s.x;
  out->size.height = layer.size.height * s.y;
  return true;
}

/**
 * @brief The size @p layer's view was laid out at, before any app scale.
 *
 * For a layer that went through @c ScaleLayer, this undoes it; for any
 * other layer it is the layer's size. This is the size a producer should
 * render at: a view under a scale animation keeps one buffer size, and the
 * compositor scales it to the drawn rect.
 */
inline FlutterSize ContentSize(const FlutterLayer& layer,
                               const double pixel_ratio) {
  const AppScale s = AppScaleOf(layer, pixel_ratio);
  if (s.IsIdentity()) {
    return layer.size;
  }
  // Snap away the rounding a scale and its inverse leave behind, so an
  // integral size does not come back a hair under and truncate one short.
  const auto snap = [](const double v) {
    const double r = std::round(v);
    return std::abs(v - r) < 1e-6 ? r : v;
  };
  return {layer.size.width > 0.0 ? snap(layer.size.width / s.x) : 0.0,
          layer.size.height > 0.0 ? snap(layer.size.height / s.y) : 0.0};
}

/**
 * @brief The opacity of @p layer's view, in [0, 1].
 *
 * The product of every opacity mutation on the view (an Opacity or
 * FadeTransition widget around it). 1 for a layer that is not a platform view,
 * has no mutations, or whose opacity is within rounding of 1, so a caller can
 * keep its opaque path for that case. Not a number reads as 1.
 */
inline float OpacityOf(const FlutterLayer& layer) {
  if (layer.type != kFlutterLayerContentTypePlatformView ||
      layer.platform_view == nullptr ||
      layer.platform_view->mutations_count == 0) {
    return 1.0f;
  }
  const double o = MutationStack::Compose(layer.platform_view).opacity;
  if (!std::isfinite(o) || o >= 1.0 - 1e-6) {
    return 1.0f;
  }
  return o <= 0.0 ? 0.0f : static_cast<float>(o);
}

/**
 * @brief The rounded clip of @p composed, for a view whose bounds on the
 * output are @p view (output pixels). See @c RoundedClipOf.
 */
inline std::optional<RoundedRect> RoundedClipIn(
    const ComposedMutation& composed,
    const FlutterRect& view) {
  if (!composed.has_rounded_mask) {
    return std::nullopt;
  }
  const FlutterRoundedRect& rr = composed.rounded_clip;
  const std::array<FlutterSize, 4> radii = {
      rr.upper_left_corner_radius, rr.upper_right_corner_radius,
      rr.lower_right_corner_radius, rr.lower_left_corner_radius};
  for (const FlutterSize& r : radii) {
    if (!std::isfinite(r.width) || !std::isfinite(r.height)) {
      return std::nullopt;
    }
  }
  const FlutterRect& c = rr.rect;
  if (!std::isfinite(c.left) || !std::isfinite(c.top) ||
      !std::isfinite(c.right) || !std::isfinite(c.bottom)) {
    return std::nullopt;
  }
  // The part of the view that shows: its rect inside the clip rect, which
  // holds this clip's bounding box.
  const FlutterRect& cr = composed.clip_rect;
  const double x0 = std::max(view.left, cr.left);
  const double y0 = std::max(view.top, cr.top);
  const double x1 = std::min(view.right, cr.right);
  const double y1 = std::min(view.bottom, cr.bottom);
  if (x1 <= x0 || y1 <= y0) {
    return std::nullopt;  // nothing shows; the clip rect drops it
  }
  // A corner cuts in when the shown part reaches into its square, the radii
  // across from the clip's own corner. Half a pixel of slack: a corner that
  // small is not drawn.
  constexpr double kSlack = 0.5;
  const bool ul = x0 < c.left + radii[0].width - kSlack &&
                  y0 < c.top + radii[0].height - kSlack;
  const bool ur = x1 > c.right - radii[1].width + kSlack &&
                  y0 < c.top + radii[1].height - kSlack;
  const bool lr = x1 > c.right - radii[2].width + kSlack &&
                  y1 > c.bottom - radii[2].height + kSlack;
  const bool ll = x0 < c.left + radii[3].width - kSlack &&
                  y1 > c.bottom - radii[3].height + kSlack;
  if (!ul && !ur && !lr && !ll) {
    return std::nullopt;
  }
  RoundedRect out;
  out.rect = {c.left, c.top, c.right - c.left, c.bottom - c.top};
  for (size_t i = 0; i < radii.size(); ++i) {
    out.radii[i] = {static_cast<float>(std::max(0.0, radii[i].width)),
                    static_cast<float>(std::max(0.0, radii[i].height))};
  }
  return out;
}

/**
 * @brief The rounded clip on @p layer's view, in output pixels.
 *
 * Empty when the view has none, or when its corners do not cut into the part
 * of the view that shows (the view's rect inside its clip rect), so a caller
 * can keep its unrounded path for that case. The clip's bounding box is part
 * of @c ClipRectOf either way; this adds only the corners.
 *
 * With more than one rounded clip, or one under a rotation or skew, this is
 * the innermost one still a rounded rect on screen, and some corners are
 * drawn square (see @c ComposedMutation::rounded_clip_exact).
 */
inline std::optional<RoundedRect> RoundedClipOf(const FlutterLayer& layer) {
  if (layer.type != kFlutterLayerContentTypePlatformView ||
      layer.platform_view == nullptr ||
      layer.platform_view->mutations_count == 0) {
    return std::nullopt;
  }
  return RoundedClipIn(
      MutationStack::Compose(layer.platform_view),
      {layer.offset.x, layer.offset.y, layer.offset.x + layer.size.width,
       layer.offset.y + layer.size.height});
}

/**
 * @brief The clip of @p composed, for a view whose bounds on the output
 * are @p view (output pixels). See @c ClipRectOf.
 */
inline std::optional<RectI> ClipRectIn(const ComposedMutation& composed,
                                       const FlutterRect& view) {
  if (!composed.has_clip) {
    return std::nullopt;
  }
  const FlutterRect& c = composed.clip_rect;
  if (!std::isfinite(c.left) || !std::isfinite(c.top) ||
      !std::isfinite(c.right) || !std::isfinite(c.bottom)) {
    return std::nullopt;
  }
  const double vx0 = view.left;
  const double vy0 = view.top;
  const double vx1 = view.right;
  const double vy1 = view.bottom;
  // Edges compared after rounding, the way they are drawn.
  const auto px = [](const double v) {
    return static_cast<int32_t>(std::lround(v));
  };
  const int32_t x0 = px(c.left);
  const int32_t y0 = px(c.top);
  const int32_t x1 = px(c.right);
  const int32_t y1 = px(c.bottom);
  if (x0 <= px(vx0) && y0 <= px(vy0) && x1 >= px(vx1) && y1 >= px(vy1)) {
    return std::nullopt;  // the view is inside its clip
  }
  if (x1 <= x0 || y1 <= y0) {
    return RectI{0, 0, 0, 0};
  }
  return RectI{x0, y0, x1 - x0, y1 - y0};
}

/**
 * @brief The clip on @p layer's view, in output pixels.
 *
 * Empty when the view has no clip, or the clip does not cut into the view's
 * rect, so a caller can keep its unclipped path for that case. A clip that
 * misses the view entirely is returned as an empty rect, which clips the view
 * away. Edges are rounded to the nearest pixel.
 *
 * A clip under a rotation or skew is not a rect on screen; its bounding box is
 * used (see @c ComposedMutation::clip_exact).
 */
inline std::optional<RectI> ClipRectOf(const FlutterLayer& layer) {
  if (layer.type != kFlutterLayerContentTypePlatformView ||
      layer.platform_view == nullptr ||
      layer.platform_view->mutations_count == 0) {
    return std::nullopt;
  }
  return ClipRectIn(
      MutationStack::Compose(layer.platform_view),
      {layer.offset.x, layer.offset.y, layer.offset.x + layer.size.width,
       layer.offset.y + layer.size.height});
}

/// A view that reaches the output through a rotation, skew, mirror or
/// perspective.
struct TransformedView {
  /// The view's own pixels to output pixels.
  ViewMap map;
  /// The view's size in its own pixels: the size the engine gives its layer.
  int32_t width{0};
  int32_t height{0};
  /// Its clip and rounded clip in output pixels, as from @c ClipRectOf and
  /// @c RoundedClipOf but tested against where the view really is.
  std::optional<RectI> clip;
  std::optional<RoundedRect> rounded;
};

/**
 * @brief Whether @p layer's view is under a rotation, skew, mirror or
 * perspective, which no rect on the output can show. A caller deciding whether
 * a rect will do (a plane, a blit) needs only this; @c TransformedViewOf gives
 * the map.
 */
inline bool IsTransformed(const FlutterLayer& layer) {
  if (layer.type != kFlutterLayerContentTypePlatformView ||
      layer.platform_view == nullptr ||
      layer.platform_view->mutations_count == 0) {
    return false;
  }
  const ComposedMutation composed = MutationStack::Compose(layer.platform_view);
  return composed.has_perspective || !composed.IsAxisAligned();
}

/**
 * @brief How @p layer's view reaches the output, when no rect can show it.
 *
 * The engine gives the layer the origin of the transformed view's bounding box
 * and the size it was laid out at, which is right only for a positive
 * axis-aligned scale (@c ScaleLayer). Under a rotation, skew or mirror the view
 * is a parallelogram, and under a perspective a quad, and this is the map that
 * draws it, from the composed transform: that maps the view's logical rect, at
 * its own origin, to output pixels, so the map from its own pixels divides out
 * the pixel ratio.
 *
 * Empty for any other view, when the pixel ratio is unknown, when the
 * transform flattens the view, and when a corner of the view is past the
 * horizon of a perspective (drawn flat, as before).
 */
inline std::optional<TransformedView> TransformedViewOf(
    const FlutterLayer& layer,
    const double pixel_ratio) {
  if (layer.type != kFlutterLayerContentTypePlatformView ||
      layer.platform_view == nullptr ||
      layer.platform_view->mutations_count == 0 ||
      !std::isfinite(pixel_ratio) || pixel_ratio <= 0.0) {
    return std::nullopt;
  }
  const ComposedMutation composed = MutationStack::Compose(layer.platform_view);
  if (!composed.has_perspective && composed.IsAxisAligned()) {
    return std::nullopt;
  }
  // The pixel ratio scales the view's own x and y, so it divides the
  // matrix's first two columns.
  TransformedView v;
  v.map.a = composed.a / pixel_ratio;
  v.map.b = composed.b / pixel_ratio;
  v.map.g = composed.g / pixel_ratio;
  v.map.c = composed.c / pixel_ratio;
  v.map.d = composed.d / pixel_ratio;
  v.map.h = composed.h / pixel_ratio;
  v.map.tx = composed.tx;
  v.map.ty = composed.ty;
  v.map.i = composed.i;
  const ViewMap& m = v.map;
  const double det = m.a * (m.d * m.i - m.ty * m.h) -
                     m.c * (m.b * m.i - m.ty * m.g) +
                     m.tx * (m.b * m.h - m.d * m.g);
  for (const double e : {m.a, m.b, m.c, m.d, m.tx, m.ty, m.g, m.h, m.i}) {
    if (!std::isfinite(e)) {
      return std::nullopt;
    }
  }
  if (std::abs(det) < 1e-9) {
    return std::nullopt;
  }
  if (!std::isfinite(layer.size.width) || !std::isfinite(layer.size.height) ||
      layer.size.width < 1.0 || layer.size.height < 1.0 ||
      layer.size.width > 1 << 24 || layer.size.height > 1 << 24) {
    return std::nullopt;
  }
  v.width = static_cast<int32_t>(std::lround(layer.size.width));
  v.height = static_cast<int32_t>(std::lround(layer.size.height));
  // Its bounds on the output, for the clips' tests.
  FlutterRect bounds{
      std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
      -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
  for (const auto& [x, y] :
       {std::pair{0, 0}, std::pair{v.width, 0}, std::pair{0, v.height},
        std::pair{v.width, v.height}}) {
    if (!(m.W(x, y) > 1e-9)) {
      return std::nullopt;  // past the horizon: no bounds, nothing to draw
    }
    const auto p = m.Map(x, y);
    bounds.left = std::min(bounds.left, p[0]);
    bounds.top = std::min(bounds.top, p[1]);
    bounds.right = std::max(bounds.right, p[0]);
    bounds.bottom = std::max(bounds.bottom, p[1]);
  }
  v.clip = ClipRectIn(composed, bounds);
  v.rounded = RoundedClipIn(composed, bounds);
  return v;
}

/**
 * @brief Scales a frame's platform view layers to their drawn size.
 *
 * Holds the copies, so one instance serves one thread: the raster thread.
 */
class FrameScaler {
 public:
  /**
   * @brief @p layers when no view in them is scaled; otherwise a copy of the
   * frame with each scaled view's size corrected, valid until the next call.
   */
  const FlutterLayer** Scale(const FlutterLayer** layers,
                             const size_t count,
                             const double pixel_ratio) {
    if (layers == nullptr || count == 0) {
      return layers;
    }
    // Most frames scale nothing: find the first scaled view before copying.
    size_t first = count;
    FlutterLayer scaled{};
    for (size_t i = 0; i < count; ++i) {
      if (layers[i] != nullptr &&
          ScaleLayer(*layers[i], pixel_ratio, &scaled)) {
        first = i;
        break;
      }
    }
    if (first == count) {
      return layers;
    }
    // Sized once, before any pointer into it is taken.
    layers_.resize(count);
    ptrs_.resize(count);
    for (size_t i = 0; i < count; ++i) {
      if (layers[i] == nullptr) {
        ptrs_[i] = nullptr;
        continue;
      }
      if (i == first) {
        layers_[i] = scaled;
      } else if (i < first ||
                 !ScaleLayer(*layers[i], pixel_ratio, &layers_[i])) {
        layers_[i] = *layers[i];
      }
      ptrs_[i] = &layers_[i];
    }
    return ptrs_.data();
  }

 private:
  std::vector<FlutterLayer> layers_;
  std::vector<const FlutterLayer*> ptrs_;
};

}  // namespace ihs::platform_view
