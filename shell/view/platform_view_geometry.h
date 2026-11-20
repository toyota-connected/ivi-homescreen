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
#include <optional>
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
 * Only positive axis-aligned transforms are corrected here. Rotation, skew,
 * mirroring and perspective are left as the engine reports them.
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
  const ComposedMutation composed = MutationStack::Compose(layer.platform_view);
  if (!composed.has_clip) {
    return std::nullopt;
  }
  const FlutterRect& c = composed.clip_rect;
  if (!std::isfinite(c.left) || !std::isfinite(c.top) ||
      !std::isfinite(c.right) || !std::isfinite(c.bottom)) {
    return std::nullopt;
  }
  const double vx0 = layer.offset.x;
  const double vy0 = layer.offset.y;
  const double vx1 = vx0 + layer.size.width;
  const double vy1 = vy0 + layer.size.height;
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
