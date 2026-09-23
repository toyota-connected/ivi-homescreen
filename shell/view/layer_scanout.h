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
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "view/layer_geometry.h"

// What the drm-kms backends need to put one layer of a platform view on its own
// KMS plane: the plane's source crop, destination and rotation, derived from
// the layer's geometry the same way the composite paths derive their texture
// coordinates. Free of DRM includes so it can be unit tested on its own; the
// constants below are the kernel's uAPI values, which the backends check
// against <drm_mode.h>.

// DRM_MODE_ROTATE_* and DRM_MODE_REFLECT_X.
constexpr uint32_t kDrmRotate0 = 1U << 0;
constexpr uint32_t kDrmRotate90 = 1U << 1;
constexpr uint32_t kDrmRotate180 = 1U << 2;
constexpr uint32_t kDrmRotate270 = 1U << 3;
constexpr uint32_t kDrmReflectX = 1U << 4;

// The plane rotation that undoes @p transform. KMS rotates the source
// counter-clockwise and reflects before it rotates; a wl_output transform
// names what the producer did, so its inverse is wanted. At 90 degrees the
// producer turned its content counter-clockwise, which takes a clockwise turn
// -- ROTATE_270 -- to undo. The reflected transforms keep their angle, since a
// reflection reverses the sense of the rotation after it.
constexpr uint32_t DrmRotationFor(const BufferTransform transform) {
  switch (transform) {
    case BufferTransform::kNormal:
      return kDrmRotate0;
    case BufferTransform::k90:
      return kDrmRotate270;
    case BufferTransform::k180:
      return kDrmRotate180;
    case BufferTransform::k270:
      return kDrmRotate90;
    case BufferTransform::kFlipped:
      return kDrmRotate0 | kDrmReflectX;
    case BufferTransform::kFlipped90:
      return kDrmRotate90 | kDrmReflectX;
    case BufferTransform::kFlipped180:
      return kDrmRotate180 | kDrmReflectX;
    case BufferTransform::kFlipped270:
      return kDrmRotate270 | kDrmReflectX;
  }
  return kDrmRotate0;
}

// A plane's source crop in 16.16 fixed point, the kernel's SRC_* encoding.
struct FixedSrcRect {
  uint32_t x{0};
  uint32_t y{0};
  uint32_t w{0};
  uint32_t h{0};
};

// Where and how one layer lands on a plane.
struct LayerPlane {
  FixedSrcRect src;  // in the buffer's own pixels, before rotation
  RectI dst;         // CRTC pixels
  uint32_t rotation{kDrmRotate0};
};

// Place a layer of a view whose rect on the CRTC is @p view. @p src and @p dst
// are the layer's geometry as the surface reports it: @p dst in view-local
// pixels, a zero-size @p dst meaning the whole view and a zero-size @p src the
// whole buffer. The layer is clipped to the view and its crop shrunk to match.
// False when nothing of it is visible, or the crop rounds away to nothing.
inline bool PlaceLayerOnPlane(RectF src,
                              RectI dst,
                              const BufferTransform transform,
                              const uint32_t buffer_w,
                              const uint32_t buffer_h,
                              const RectI& view,
                              LayerPlane* out) {
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
  // Keep the crop inside the buffer. A producer may name one that is not; the
  // kernel rejects such a plane outright, which would take the whole frame to
  // the composite path over one bad layer.
  const double bw = buffer_w;
  const double bh = buffer_h;
  const double x0 = std::clamp(src.x, 0.0, bw);
  const double y0 = std::clamp(src.y, 0.0, bh);
  const double x1 = std::clamp(src.x + src.w, 0.0, bw);
  const double y1 = std::clamp(src.y + src.h, 0.0, bh);
  const auto fixed = [](const double v) {
    return static_cast<uint32_t>(std::llround(v * 65536.0));
  };
  out->src = {fixed(x0), fixed(y0), fixed(x1) - fixed(x0),
              fixed(y1) - fixed(y0)};
  if (out->src.w == 0 || out->src.h == 0) {
    return false;
  }
  out->dst = {view.x + dst.x, view.y + dst.y, dst.w, dst.h};
  out->rotation = DrmRotationFor(transform);
  return true;
}

// The format to scan an opaque layer out as. An opaque layer's alpha channel
// is undefined, and a plane blends with it; the same memory read as the
// channel-for-channel format without alpha shows it as it is meant to be seen.
// Formats with no such twin are returned unchanged.
constexpr uint32_t OpaqueScanoutFourcc(const uint32_t fourcc) {
  constexpr auto code = [](const char a, const char b, const char c,
                           const char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
  };
  constexpr std::pair<uint32_t, uint32_t> kPairs[] = {
      {code('A', 'R', '2', '4'), code('X', 'R', '2', '4')},  // ARGB8888
      {code('A', 'B', '2', '4'), code('X', 'B', '2', '4')},  // ABGR8888
      {code('R', 'A', '2', '4'), code('R', 'X', '2', '4')},  // RGBA8888
      {code('B', 'A', '2', '4'), code('B', 'X', '2', '4')},  // BGRA8888
      {code('A', 'R', '3', '0'), code('X', 'R', '3', '0')},  // ARGB2101010
      {code('A', 'B', '3', '0'), code('X', 'B', '3', '0')},  // ABGR2101010
      {code('A', 'R', '1', '5'), code('X', 'R', '1', '5')},  // ARGB1555
      {code('A', 'R', '1', '2'), code('X', 'R', '1', '2')},  // ARGB4444
      {code('A', 'B', '4', 'H'), code('X', 'B', '4', 'H')},  // ABGR16161616F
  };
  for (const auto& [with_alpha, without] : kPairs) {
    if (fourcc == with_alpha) {
      return without;
    }
  }
  return fourcc;
}

// Scene identity tags for the layers of platform views. A scene layer needs a
// stable, unique pointer to be found by from one present to the next, and a
// view now has one scene layer per layer_id, so the view's own pointer no
// longer does. Each tag is a small allocation that lives as long as its scene
// layer. @p surface is only compared, never dereferenced. Raster thread only.
class PvLayerTags {
 public:
  // The tag for layer @p layer_id of @p surface, made on first use.
  void* Get(const void* surface, const uint32_t layer_id) {
    auto& tag = tags_[{surface, layer_id}];
    if (!tag) {
      tag = std::make_unique<Tag>(Tag{surface, layer_id});
    }
    return tag.get();
  }

  // Every tag made for @p surface, for work that spans all of a view's
  // layers.
  [[nodiscard]] std::vector<void*> TagsOf(const void* surface) const {
    std::vector<void*> out;
    for (auto it = tags_.lower_bound({surface, 0});
         it != tags_.end() && it->first.first == surface; ++it) {
      out.push_back(it->second.get());
    }
    return out;
  }

  // Forget every tag not in @p keep. The caller has removed their scene
  // layers already.
  template <typename Ptr>
  void RetainOnly(const std::vector<Ptr>& keep) {
    for (auto it = tags_.begin(); it != tags_.end();) {
      if (std::find(keep.begin(), keep.end(), it->second.get()) == keep.end()) {
        it = tags_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void Clear() { tags_.clear(); }

  [[nodiscard]] size_t size() const { return tags_.size(); }

 private:
  struct Tag {
    const void* surface;
    uint32_t layer_id;
  };
  std::map<std::pair<const void*, uint32_t>, std::unique_ptr<Tag>> tags_;
};
