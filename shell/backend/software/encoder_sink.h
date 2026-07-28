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

// The NV12 consumer seam (INv12Consumer + MakeNv12Consumer) is backend-neutral,
// so the CPU EncoderSink here and the GPU headless-EGL backend share it.
#include "backend/software/nv12_consumer.h"
#include "backend/software/surface_sink.h"

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
