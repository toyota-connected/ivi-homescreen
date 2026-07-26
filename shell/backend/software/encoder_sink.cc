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
#include <ctime>

#include "logging/logging.h"

namespace {

// Real elapsed time for the frame timestamp, so the encoder's PTS tracks the
// actual present cadence rather than an assumed frame rate.
uint64_t MonotonicUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000 +
         static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

// Allocate one dma-buf for the packed NV12 frame. Prefers a CMA heap (the
// physically contiguous memory most SoC V4L2 encoders require); the `system`
// heap is a last-resort fallback and is not necessarily contiguous. Returns the
// fd, or -1.
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
  // Destroy the consumer first so no OnFrame is in progress and every held
  // frame's release has fired before we unmap/close the ring it points into.
  consumer_.reset();
  ReleaseBuffer();
}

void EncoderSink::ReleaseSlotTrampoline(void* ctx) {
  auto* ref = static_cast<SlotRef*>(ctx);
  ref->sink->ReleaseSlot(ref->index);
}

void EncoderSink::ReleaseSlot(uint32_t index) {
  std::lock_guard<std::mutex> lock(ring_mu_);
  ring_[index].in_flight = false;
}

void EncoderSink::ReleaseBuffer() {
  for (auto& slot : ring_) {
    if (slot.map != nullptr) {
      ::munmap(slot.map, slot_size_);
    }
    if (slot.fd >= 0) {
      ::close(slot.fd);
    }
    slot = {};
  }
  slot_size_ = 0;
  ready_ = false;
}

bool EncoderSink::EnsureBuffer(uint32_t width, uint32_t height) {
  // NV12 is subsampled 2x2, so both dimensions must be even. Round an odd
  // source down -- the last column/row is dropped from the encode -- and match
  // the fast path on the rounded values; comparing the raw input against the
  // (already rounded) width_/height_ would miss for an odd source and force a
  // ReleaseBuffer()+realloc on every frame.
  const uint32_t even_w = width & ~1u;
  const uint32_t even_h = height & ~1u;
  if (ready_ && even_w == width_ && even_h == height_) {
    return true;
  }
  // A geometry change must not free a slot an async consumer still holds --
  // unmapping/closing it mid-encode would invalidate the fd/mapping in use and
  // race the later release. Defer the realloc (drop this frame) until every
  // in-flight slot has been released; the encoder drains them within a few
  // frames. (The dtor is safe already: it destroys the consumer first.)
  {
    std::lock_guard<std::mutex> lock(ring_mu_);
    for (const auto& slot : ring_) {
      if (slot.in_flight) {
        return false;
      }
    }
  }
  ReleaseBuffer();
  if ((width & 1u) != 0 || (height & 1u) != 0) {
    ihs::log::warn(
        "[EncoderSink] source {}x{} has an odd dimension; encoding {}x{} "
        "(last column/row dropped)",
        width, height, even_w, even_h);
  }
  width_ = even_w;
  height_ = even_h;
  stride_ = width_;
  if (width_ == 0 || height_ == 0) {
    return false;
  }
  slot_size_ = static_cast<size_t>(stride_) * height_ * 3 / 2;

  for (uint32_t i = 0; i < kRingSize; ++i) {
    ring_[i].fd = AllocDmabuf(slot_size_);
    if (ring_[i].fd < 0) {
      ihs::log::warn("[EncoderSink] dma-heap alloc of {} bytes failed",
                     slot_size_);
      ReleaseBuffer();
      return false;
    }
    ring_[i].map = static_cast<uint8_t*>(::mmap(nullptr, slot_size_,
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED, ring_[i].fd, 0));
    if (ring_[i].map == MAP_FAILED) {
      ring_[i].map = nullptr;
      ihs::log::warn("[EncoderSink] mmap of NV12 dma-buf failed: {}",
                     std::strerror(errno));
      ReleaseBuffer();
      return false;
    }
    ring_[i].in_flight = false;
    slot_ref_[i] = {this, i};
  }
  if (consumer_ == nullptr || !consumer_->Configure(width_, height_, stride_)) {
    ihs::log::warn("[EncoderSink] consumer rejected {}x{}", width_, height_);
    ReleaseBuffer();
    return false;
  }
  ready_ = true;
  // Force a keyframe on the next Present: the consumer was (re)configured, so a
  // resize starts a fresh encoder that must open with an IDR for a decoder to
  // resync at the new geometry. Resetting here covers every (re)configure, not
  // just the first buffer.
  frame_index_ = 0;
  ihs::log::info("[EncoderSink] NV12 dma-buf ready ({}x{}, stride {})", width_,
                 height_, stride_);
  return true;
}

void EncoderSink::OnSize(uint32_t width, uint32_t height) {
  have_onsize_ = true;
  onsize_width_ = width;
  EnsureBuffer(width, height);
}

bool EncoderSink::Present(const void* allocation,
                          size_t row_bytes,
                          size_t height) {
  if (allocation == nullptr) {
    return true;
  }
  // row_bytes is the source stride. Prefer the OnSize width (row_bytes can
  // exceed 4*width as padding, which we must not encode as image), but never
  // encode more columns than the row actually holds -- clamp to row_bytes/4 so
  // a short row can't be read past its end.
  const uint32_t stride_px = static_cast<uint32_t>(row_bytes / 4);
  const uint32_t src_h = static_cast<uint32_t>(height);
  const uint32_t src_w =
      have_onsize_ ? std::min(onsize_width_, stride_px) : stride_px;
  if (!EnsureBuffer(src_w, src_h)) {
    return true;  // soft failure: keep the engine running
  }

  // Take a free ring slot; drop the frame if the consumer has released none
  // (favor latency over a growing backlog).
  uint32_t idx = kRingSize;
  {
    std::lock_guard<std::mutex> lock(ring_mu_);
    for (uint32_t i = 0; i < kRingSize; ++i) {
      if (!ring_[i].in_flight) {
        idx = i;
        break;
      }
    }
    if (idx == kRingSize) {
      if (++dropped_ % 60 == 1) {
        ihs::log::warn(
            "[EncoderSink] ring full; dropping frames (consumer behind, {} "
            "dropped)",
            dropped_);
      }
      return true;
    }
    ring_[idx].in_flight = true;
  }

  Slot& slot = ring_[idx];
  DmaSyncWrite(slot.fd, /*start=*/true);
  BgraToNv12(static_cast<const uint8_t*>(allocation), width_, height_,
             row_bytes, stride_, slot.map);
  DmaSyncWrite(slot.fd, /*start=*/false);

  // Real capture time; the first frame after (re)configure forces a keyframe.
  const bool took = consumer_->OnFrame(slot.fd, slot.map, MonotonicUs(),
                                       /*force_keyframe=*/frame_index_ == 0,
                                       &ReleaseSlotTrampoline, &slot_ref_[idx]);
  ++frame_index_;
  if (!took) {
    ReleaseSlot(idx);  // synchronous consumer: reclaim the slot now
  }
  return true;
}
