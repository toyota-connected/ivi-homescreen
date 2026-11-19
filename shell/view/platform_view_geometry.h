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
#include <vector>

#include <shell/platform/embedder/embedder.h>

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
