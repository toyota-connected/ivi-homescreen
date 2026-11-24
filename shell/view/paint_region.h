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
#include <cstdint>
#include <optional>
#include <vector>

#include <shell/platform/embedder/embedder.h>

#include "view/layer_geometry.h"

// Which part of a backing store Flutter painted. The engine reports it with
// each backing-store layer (FlutterBackingStorePresentInfo::paint_region), and
// everything outside it is transparent, so a compositor blending the store over
// what is below it need only copy and draw those rects. An overlay store above
// a platform view is full-screen but usually holds a few small widgets.

// The most rects drawn one by one. Past it, their bounding box is drawn: a
// region of many small rects costs more in draws than it saves in pixels.
inline constexpr size_t kMaxPaintRects = 16;

// The painted rects of @p layer, in the store's own pixels, rounded out to
// whole pixels and clipped to its @p store_w x @p store_h. Empty when nothing
// was painted, so the store can be skipped. Unknown -- no region reported,
// or one that cannot be trusted -- when the whole store has to be drawn.
inline std::optional<std::vector<RectI>> PaintRects(const FlutterLayer& layer,
                                                    const int32_t store_w,
                                                    const int32_t store_h) {
  if (layer.type != kFlutterLayerContentTypeBackingStore ||
      layer.backing_store_present_info == nullptr ||
      layer.backing_store_present_info->struct_size <
          sizeof(FlutterBackingStorePresentInfo) ||
      layer.backing_store_present_info->paint_region == nullptr ||
      store_w <= 0 || store_h <= 0) {
    return std::nullopt;
  }
  const FlutterRegion& region = *layer.backing_store_present_info->paint_region;
  if (region.struct_size < sizeof(FlutterRegion) ||
      (region.rects_count > 0 && region.rects == nullptr)) {
    return std::nullopt;
  }
  std::vector<RectI> out;
  out.reserve(std::min(region.rects_count, kMaxPaintRects));
  for (size_t i = 0; i < region.rects_count; ++i) {
    const FlutterRect& r = region.rects[i];
    if (!std::isfinite(r.left) || !std::isfinite(r.top) ||
        !std::isfinite(r.right) || !std::isfinite(r.bottom)) {
      return std::nullopt;
    }
    const auto x0 = static_cast<int32_t>(
        std::clamp(std::floor(r.left), 0.0, static_cast<double>(store_w)));
    const auto y0 = static_cast<int32_t>(
        std::clamp(std::floor(r.top), 0.0, static_cast<double>(store_h)));
    const auto x1 = static_cast<int32_t>(
        std::clamp(std::ceil(r.right), 0.0, static_cast<double>(store_w)));
    const auto y1 = static_cast<int32_t>(
        std::clamp(std::ceil(r.bottom), 0.0, static_cast<double>(store_h)));
    if (x1 > x0 && y1 > y0) {
      out.push_back({x0, y0, x1 - x0, y1 - y0});
    }
  }
  if (out.size() > kMaxPaintRects) {
    RectI box = out.front();
    for (const RectI& r : out) {
      const int32_t x1 = std::max(box.x + box.w, r.x + r.w);
      const int32_t y1 = std::max(box.y + box.h, r.y + r.h);
      box.x = std::min(box.x, r.x);
      box.y = std::min(box.y, r.y);
      box.w = x1 - box.x;
      box.h = y1 - box.y;
    }
    out.assign(1, box);
  }
  return out;
}

// @p r grown by @p pad pixels on each side, inside a @p w x @p h store: what
// to copy so that filtering at the rect's edge reads only copied texels.
inline RectI PadRect(const RectI& r,
                     const int32_t pad,
                     const int32_t w,
                     const int32_t h) {
  const int32_t x0 = std::max(r.x - pad, 0);
  const int32_t y0 = std::max(r.y - pad, 0);
  const int32_t x1 = std::min(r.x + r.w + pad, w);
  const int32_t y1 = std::min(r.y + r.h + pad, h);
  return {x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0)};
}

// The texture coordinates that draw the store rect @p r of a @p w x @p h store
// over a destination covering just that rect.
inline UvAffine UvForRect(const RectI& r, const int32_t w, const int32_t h) {
  UvAffine uv;
  uv.a = static_cast<float>(r.w) / static_cast<float>(w);
  uv.d = static_cast<float>(r.h) / static_cast<float>(h);
  uv.tx = static_cast<float>(r.x) / static_cast<float>(w);
  uv.ty = static_cast<float>(r.y) / static_cast<float>(h);
  return uv;
}
