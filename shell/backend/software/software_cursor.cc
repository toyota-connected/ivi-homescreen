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

#include "backend/software/software_cursor.h"

#include <cstring>

namespace {

// Classic left-pointer arrow, 12x19. 'o' = black outline, 'X' = white fill,
// space = transparent. Hotspot is the tip at (0, 0). Each row is exactly
// kCursorW characters.
constexpr uint32_t kCursorW = 12;
constexpr uint32_t kCursorH = 19;
constexpr const char* kArrow[kCursorH] = {
    "o           ", "oo          ", "oXo         ", "oXXo        ",
    "oXXXo       ", "oXXXXo      ", "oXXXXXo     ", "oXXXXXXo    ",
    "oXXXXXXXo   ", "oXXXXXXXXo  ", "oXXXXXoooooo", "oXXoXXo     ",
    "oXo oXXo    ", "oo  oXXo    ", "o    oXXo   ", "     oXXo   ",
    "      oXXo  ", "      oXXo  ", "       oo   ",
};

// Premultiplied ARGB8888.
constexpr uint32_t kFill = 0xFFFFFFFF;     // opaque white
constexpr uint32_t kOutline = 0xFF000000;  // opaque black
constexpr uint32_t kClear = 0x00000000;    // transparent

}  // namespace

SoftwareCursor::SoftwareCursor()
    : width_(kCursorW), height_(kCursorH), hot_x_(0), hot_y_(0) {
  pixels_.resize(static_cast<size_t>(width_) * height_, kClear);
  for (uint32_t row = 0; row < height_; ++row) {
    const char* line = kArrow[row];
    const size_t len = std::strlen(line);
    for (uint32_t col = 0; col < width_; ++col) {
      const char c = col < len ? line[col] : ' ';
      uint32_t argb = kClear;
      if (c == 'X') {
        argb = kFill;
      } else if (c == 'o') {
        argb = kOutline;
      }
      pixels_[static_cast<size_t>(row) * width_ + col] = argb;
    }
  }
}

void SoftwareCursor::BlendXRGB(uint8_t* map,
                               const uint32_t fb_w,
                               const uint32_t fb_h,
                               const uint32_t pitch) const {
  if (map == nullptr || !visible_.load(std::memory_order_acquire)) {
    return;
  }
  const std::lock_guard<std::mutex> lock(bitmap_mtx_);
  const int32_t ox = x_.load(std::memory_order_relaxed) - hot_x_;
  const int32_t oy = y_.load(std::memory_order_relaxed) - hot_y_;

  for (uint32_t cy = 0; cy < height_; ++cy) {
    const int32_t dy = oy + static_cast<int32_t>(cy);
    if (dy < 0 || dy >= static_cast<int32_t>(fb_h)) {
      continue;
    }
    for (uint32_t cx = 0; cx < width_; ++cx) {
      const int32_t dx = ox + static_cast<int32_t>(cx);
      if (dx < 0 || dx >= static_cast<int32_t>(fb_w)) {
        continue;
      }
      const uint32_t src = pixels_[static_cast<size_t>(cy) * width_ + cx];
      const uint32_t a = src >> 24;
      if (a == 0) {
        continue;  // fully transparent — leave the framebuffer pixel
      }
      const uint32_t sr = (src >> 16) & 0xFF;
      const uint32_t sg = (src >> 8) & 0xFF;
      const uint32_t sb = src & 0xFF;
      const uint32_t ia = 255 - a;  // inverse alpha for the "over" blend

      // XRGB8888 little-endian: byte order in memory is [B, G, R, X].
      uint8_t* d =
          map + static_cast<size_t>(dy) * pitch + static_cast<size_t>(dx) * 4;
      d[0] = static_cast<uint8_t>(sb + (d[0] * ia) / 255);
      d[1] = static_cast<uint8_t>(sg + (d[1] * ia) / 255);
      d[2] = static_cast<uint8_t>(sr + (d[2] * ia) / 255);
      // d[3] (X) left unchanged.
    }
  }
}

void SoftwareCursor::SetShape(const uint32_t* const argb,
                              const uint32_t width,
                              const uint32_t height,
                              const int32_t hot_x,
                              const int32_t hot_y) {
  if (argb == nullptr || width == 0 || height == 0) {
    return;
  }
  const std::lock_guard<std::mutex> lock(bitmap_mtx_);
  pixels_.assign(argb, argb + static_cast<size_t>(width) * height);
  width_ = width;
  height_ = height;
  hot_x_ = hot_x;
  hot_y_ = hot_y;
}