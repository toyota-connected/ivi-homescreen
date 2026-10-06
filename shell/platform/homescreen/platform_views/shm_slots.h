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

#include <sys/types.h>

#include <array>
#include <cstdint>
#include <memory>

// The buffers of one SOFTWARE_SHM grant: LINEAR dma-bufs from GBM on the
// compositor's render node, exported read-write so the producer can mmap and
// fill them, and imported by the compositor like any other dma-buf. The
// producer writes one slot while the compositor samples the other.
//
// libgbm is loaded on first use rather than linked, so a build that does not
// otherwise need it does not depend on it. Without libgbm, or without a render
// node, Allocate() fails and the grant is refused.
//
// Each slot is one row taller than the image: some samplers read past the last
// row of an image, and reject a buffer with no room for it.
class ShmSlots {
 public:
  static constexpr uint32_t kCount = 2;

  // Slots of @width x @height in @fourcc on the render node @render_device (a
  // dev_t). @fourcc must be one BytesPerPixel() knows. Null on failure, logged.
  static std::unique_ptr<ShmSlots> Allocate(uint64_t render_device,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t fourcc);

  // The bytes per pixel of the formats a slot can hold, or 0 for any other.
  static uint32_t BytesPerPixel(uint32_t fourcc);

  ~ShmSlots();
  ShmSlots(const ShmSlots&) = delete;
  ShmSlots& operator=(const ShmSlots&) = delete;

  // Slot @index's dma-buf. Borrowed: it stays open as long as this object.
  [[nodiscard]] int fd(uint32_t index) const { return slots_[index].fd; }
  [[nodiscard]] uint32_t stride() const { return stride_; }
  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  [[nodiscard]] uint32_t fourcc() const { return fourcc_; }

  // Whether @fd is a handle to slot @index's buffer: the same dma-buf, not
  // merely a dma-buf.
  [[nodiscard]] bool Matches(uint32_t index, int fd) const;

 private:
  ShmSlots() = default;

  struct Slot {
    int fd{-1};
    dev_t dev{0};
    ino_t ino{0};
  };
  std::array<Slot, kCount> slots_{};
  uint32_t stride_{0};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t fourcc_{0};
};
