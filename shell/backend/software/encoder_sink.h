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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "backend/software/surface_sink.h"

// Destination for the NV12 frames EncoderSink packs. One implementation drives
// the V4L2 hardware encoder to a file (validation); another will feed a WebRTC
// send track. The seam keeps libwebrtc and the V4L2 codec out of the sink TU,
// so the minimal software backend stays dependency-light and only the consumer
// TU (compiled under BUILD_SOFTWARE_SINK_ENCODER) links them.
class INv12Consumer {
 public:
  virtual ~INv12Consumer() = default;

  // Called once the frame geometry is known (EncoderSink::OnSize). The NV12 is
  // one contiguous buffer: Y is width x height at `stride`, interleaved CbCr
  // follows at offset stride*height. Returns false to disable the sink.
  virtual bool Configure(uint32_t width, uint32_t height, uint32_t stride) = 0;

  // Hand off one packed NV12 frame. `dmabuf_fd` is the single fd an encoder
  // imports zero-copy; `nv12` maps the same bytes for a CPU-side consumer.
  // Called on Flutter's rasterizer thread.
  //
  // The frame lives in one slot of the sink's ring. A consumer that FINISHES
  // with it before returning (a synchronous encode) returns false, and the sink
  // reclaims the slot immediately. A consumer that HOLDS it past the call (an
  // asynchronous encode -- e.g. a WebRTC send whose encoder runs on its own
  // thread) returns true, takes the release obligation, and MUST call
  // `release(release_ctx)` exactly once when done -- from any thread. Until it
  // does, that slot is not reused, so the buffer it points at stays valid.
  virtual bool OnFrame(int dmabuf_fd,
                       const uint8_t* nv12,
                       uint64_t timestamp_us,
                       bool force_keyframe,
                       void (*release)(void*),
                       void* release_ctx) = 0;
};

// Build a consumer from the part of the sink spec after "encoder:". Today:
//   file:<path>   -> encode to an H.264 Annex-B file (validation)
// Returns nullptr on an unrecognized / unbuildable spec.
std::unique_ptr<INv12Consumer> MakeNv12Consumer(std::string_view spec);

// Software-backend sink that converts each premultiplied-BGRA present to NV12
// in a dma-buf and hands it to an INv12Consumer. No networking or encoder
// dependency lives here -- that is all behind INv12Consumer. Frames are packed
// into a small ring of dma-bufs so an asynchronous consumer can still be
// encoding one frame while the next present is packed; when the ring is full
// the present is dropped (latency over a growing queue).
class EncoderSink final : public ISurfaceSink {
 public:
  explicit EncoderSink(std::unique_ptr<INv12Consumer> consumer);
  ~EncoderSink() override;

  bool Present(const void* allocation,
               size_t row_bytes,
               size_t height) override;
  void OnSize(uint32_t width, uint32_t height) override;

 private:
  static constexpr uint32_t kRingSize = 4;

  // (Re)allocate the dma-buf ring for `width`x`height`. Returns false on
  // failure (the sink then drops frames).
  bool EnsureBuffer(uint32_t width, uint32_t height);
  void ReleaseBuffer();
  // Return a ring slot to the free pool. Thread-safe: an async consumer's
  // release fires from its own (encode) thread. The trampoline is what a
  // consumer receives as its release callback.
  void ReleaseSlot(uint32_t index);
  static void ReleaseSlotTrampoline(void* ctx);

  std::unique_ptr<INv12Consumer> consumer_;

  // One NV12 dma-buf. `in_flight` is set while a consumer holds the frame.
  struct Slot {
    int fd{-1};
    uint8_t* map{nullptr};
    bool in_flight{false};
  };
  // Stable per-slot cookie handed to the consumer as release_ctx.
  struct SlotRef {
    EncoderSink* sink;
    uint32_t index;
  };
  Slot ring_[kRingSize];
  SlotRef slot_ref_[kRingSize];
  std::mutex
      ring_mu_;  // guards Slot::in_flight across the rasterizer + release

  size_t slot_size_{0};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t stride_{0};
  uint64_t frame_index_{0};
  uint64_t dropped_{0};
  bool ready_{false};
  // Authoritative render width from OnSize. Kept separate from width_ (which
  // EnsureBuffer mutates to the allocated buffer's width) so a transient short
  // row can't permanently shrink the encoded width.
  bool have_onsize_{false};
  uint32_t onsize_width_{0};
};
