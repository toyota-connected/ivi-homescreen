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

#include "backend/software/encoder_sink.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "logging/logging.h"

namespace {

// Allocate one contiguous CMA buffer as a dma-buf. Returns the fd, or -1.
int AllocDmabuf(size_t size) {
  static const char* kHeaps[] = {"/dev/dma_heap/linux,cma",
                                 "/dev/dma_heap/default_cma_region",
                                 "/dev/dma_heap/system"};
  for (const char* path : kHeaps) {
    const int heap = ::open(path, O_RDWR | O_CLOEXEC);
    if (heap < 0) {
      continue;
    }
    dma_heap_allocation_data alloc{};
    alloc.len = size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    const int rc = ::ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc);
    ::close(heap);
    if (rc == 0) {
      return static_cast<int>(alloc.fd);
    }
  }
  return -1;
}

void DmaSyncWrite(int fd, bool start) {
  dma_buf_sync sync{};
  sync.flags =
      (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_WRITE;
  if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
    // A failed CPU/device sync can leave the encoder reading stale cache lines
    // (visible as corrupt frames). Warn once rather than per frame.
    static bool warned = false;
    if (!warned) {
      ihs::log::warn(
          "[EncoderSink] DMA_BUF_IOCTL_SYNC failed ({}); frames may show cache "
          "artifacts",
          std::strerror(errno));
      warned = true;
    }
  }
}

// BgraToNv12 reads Skia's kN32 buffer as BGRA in memory, which holds on a
// little-endian host (every shipping target). Fail the build on big-endian
// rather than silently swap channels; a BE target renders kN32 as RGBA and
// would need a byte-order branch below (cf. pixel_swizzle.h's endian gate).
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "EncoderSink BGRA->NV12 assumes a little-endian host (kN32 == BGRA)"
#endif

// Premultiplied BGRA8888 (kN32 on a little-endian host: memory [B,G,R,A]) ->
// NV12, limited-range BT.601. Un-premultiplies so a translucent scene converts
// correctly; opaque pixels pass straight through. Chroma is point-subsampled
// at the 2x2 block center.
void BgraToNv12(const uint8_t* bgra,
                uint32_t w,
                uint32_t h,
                size_t row_bytes,
                uint32_t stride,
                uint8_t* nv12) {
  auto clamp8 = [](int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
  };
  auto straighten = [](int c, int a) {
    return (a == 0 || a == 255) ? c : std::min(255, c * 255 / a);
  };
  auto sample = [&](uint32_t x, uint32_t y, int& r, int& g, int& b) {
    const uint8_t* s = bgra + static_cast<size_t>(y) * row_bytes + x * 4;
    const int a = s[3];
    b = straighten(s[0], a);
    g = straighten(s[1], a);
    r = straighten(s[2], a);
  };

  uint8_t* yp = nv12;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      int r;
      int g;
      int b;
      sample(x, y, r, g, b);
      yp[static_cast<size_t>(y) * stride + x] =
          clamp8((257 * r + 504 * g + 98 * b + 16000) / 1000);
    }
  }
  uint8_t* uvp = nv12 + static_cast<size_t>(stride) * h;
  for (uint32_t cy = 0; cy < h / 2; ++cy) {
    for (uint32_t cx = 0; cx < w / 2; ++cx) {
      int r;
      int g;
      int b;
      sample(std::min(cx * 2 + 1, w - 1), std::min(cy * 2 + 1, h - 1), r, g, b);
      uint8_t* d = uvp + static_cast<size_t>(cy) * stride + cx * 2;
      d[0] = clamp8((-148 * r - 291 * g + 439 * b + 128000) / 1000);  // Cb
      d[1] = clamp8((439 * r - 368 * g - 71 * b + 128000) / 1000);    // Cr
    }
  }
}

}  // namespace

EncoderSink::EncoderSink(std::unique_ptr<INv12Consumer> consumer)
    : consumer_(std::move(consumer)) {}

EncoderSink::~EncoderSink() {
  ReleaseBuffer();
}

void EncoderSink::ReleaseBuffer() {
  if (map_ != nullptr) {
    ::munmap(map_, map_size_);
    map_ = nullptr;
  }
  if (dmabuf_fd_ >= 0) {
    ::close(dmabuf_fd_);
    dmabuf_fd_ = -1;
  }
  map_size_ = 0;
  ready_ = false;
}

bool EncoderSink::EnsureBuffer(uint32_t width, uint32_t height) {
  if (ready_ && width == width_ && height == height_) {
    return true;
  }
  ReleaseBuffer();
  // NV12 is subsampled 2x2, so both dimensions must be even. Round an odd
  // source down and say so -- the last column/row is dropped from the encode.
  if ((width & 1u) != 0 || (height & 1u) != 0) {
    ihs::log::warn(
        "[EncoderSink] source {}x{} has an odd dimension; encoding {}x{} "
        "(last column/row dropped)",
        width, height, width & ~1u, height & ~1u);
  }
  width_ = width & ~1u;
  height_ = height & ~1u;
  stride_ = width_;
  if (width_ == 0 || height_ == 0) {
    return false;
  }
  map_size_ = static_cast<size_t>(stride_) * height_ * 3 / 2;

  dmabuf_fd_ = AllocDmabuf(map_size_);
  if (dmabuf_fd_ < 0) {
    ihs::log::warn("[EncoderSink] dma-heap alloc of {} bytes failed",
                   map_size_);
    return false;
  }
  map_ = static_cast<uint8_t*>(::mmap(
      nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd_, 0));
  if (map_ == MAP_FAILED) {
    map_ = nullptr;
    ihs::log::warn("[EncoderSink] mmap of NV12 dma-buf failed");
    ReleaseBuffer();
    return false;
  }
  if (consumer_ == nullptr || !consumer_->Configure(width_, height_, stride_)) {
    ihs::log::warn("[EncoderSink] consumer rejected {}x{}", width_, height_);
    ReleaseBuffer();
    return false;
  }
  ready_ = true;
  ihs::log::info("[EncoderSink] NV12 dma-buf ready ({}x{}, stride {})", width_,
                 height_, stride_);
  return true;
}

void EncoderSink::OnSize(uint32_t width, uint32_t height) {
  have_onsize_ = true;
  EnsureBuffer(width, height);
}

bool EncoderSink::Present(const void* allocation,
                          size_t row_bytes,
                          size_t height) {
  if (allocation == nullptr) {
    return true;
  }
  // Prefer the geometry OnSize gave us. row_bytes is the source stride and can
  // exceed 4*width (padding), so deriving width from it would encode the pad
  // columns as image. Fall back to row_bytes/4 only if OnSize never fired.
  const uint32_t src_h = static_cast<uint32_t>(height);
  const uint32_t src_w =
      have_onsize_ ? width_ : static_cast<uint32_t>(row_bytes / 4);
  if (!EnsureBuffer(src_w, src_h)) {
    return true;  // soft failure: keep the engine running
  }

  DmaSyncWrite(dmabuf_fd_, /*start=*/true);
  BgraToNv12(static_cast<const uint8_t*>(allocation), width_, height_,
             row_bytes, stride_, map_);
  DmaSyncWrite(dmabuf_fd_, /*start=*/false);

  // ~30 fps timestamp; the first frame forces a keyframe.
  const uint64_t ts_us = frame_index_ * 33'333;
  consumer_->OnFrame(dmabuf_fd_, map_, ts_us,
                     /*force_keyframe=*/frame_index_ == 0);
  ++frame_index_;
  return true;
}
