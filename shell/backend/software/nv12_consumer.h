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

#include <cstdint>
#include <memory>
#include <string_view>

// The NV12-frame consumer seam, kept free of any backend so more than one
// producer can feed it: the software backend's EncoderSink packs BGRA on the
// CPU, and the headless-EGL backend packs on the GPU, but both hand the same
// NV12 dma-buf to the same consumer (a file encoder or a WebRTC send session).
class INv12Consumer {
 public:
  virtual ~INv12Consumer() = default;

  // Called once the frame geometry is known. The NV12 is one contiguous buffer:
  // Y is width x height at `stride`, interleaved CbCr follows at stride*height.
  // Returns false to disable the producer.
  virtual bool Configure(uint32_t width, uint32_t height, uint32_t stride) = 0;

  // Hand off one packed NV12 frame. `dmabuf_fd` is the single fd an encoder
  // imports zero-copy; `nv12` maps the same bytes for a CPU-side consumer.
  //
  // The frame lives in a slot of the producer's ring. A consumer that FINISHES
  // with it before returning (a synchronous encode) returns false and the
  // producer reclaims the slot immediately. A consumer that HOLDS it past the
  // call (an asynchronous encode -- e.g. a WebRTC send whose encoder runs on
  // its own thread) returns true, takes the release obligation, and MUST call
  // `release(release_ctx)` exactly once when done, from any thread.
  virtual bool OnFrame(int dmabuf_fd,
                       const uint8_t* nv12,
                       uint64_t timestamp_us,
                       bool force_keyframe,
                       void (*release)(void*),
                       void* release_ctx) = 0;
};

// Build a consumer from the part of the sink spec after "encoder:". Today:
//   file:<path>          -> encode to an H.264 Annex-B file (validation)
//   webrtc:<host>:<port> -> feed a WebRTC send session (BUILD_..._WEBRTC)
// Returns nullptr on an unrecognized / unbuildable spec.
std::unique_ptr<INv12Consumer> MakeNv12Consumer(std::string_view spec);
