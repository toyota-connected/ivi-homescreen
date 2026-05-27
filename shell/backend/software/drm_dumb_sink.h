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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include "backend/software/surface_sink.h"

class TaskRunner;

// DRM dumb-buffer sink. Drives a single CRTC + connector via the
// modesetting interface only — no GBM, no GL, no Mesa runtime.
// Suitable for CPU-only SoCs with DRM/KMS but no render-capable GPU.
//
// Buffer format is selected at Create() time from the IVI_SW_DRM_FORMAT
// env var: default (or "xrgb8888") allocates DRM_FORMAT_XRGB8888 at
// 32 bpp — universally supported on legacy CRTCs; "rgb565" allocates
// DRM_FORMAT_RGB565 at 16 bpp, halving framebuffer footprint and the
// CRTC scanout bandwidth that legacy SoCs like TI AM335x are
// bottlenecked on. If RGB565 is requested but the picked CRTC's
// planes don't advertise it, the sink warns and falls back to XRGB.
//
// Pipeline per Present():
//   1. Pick the back buffer (front buffer is currently scanning out).
//   2. Pack Flutter's BGRA8888-in-memory source into the dumb buffer
//      via pixel_swizzle.h — FlutterToBGRX8888 for XRGB, FlutterToRGB565
//      for RGB565. Both crop/pad to the panel's mode dimensions if the
//      embedder's view geometry differs.
//   3. drmModePageFlip onto the back buffer. flip_pending_ becomes
//      true; the next Present blocks until it clears.
//
// The page-flip event arrives on the platform task runner via an
// asio async_wait on the drm fd (mirrors drm_kms_egl/drm_backend.cc's
// pattern). On flip-complete the handler exchanges the parked vsync
// baton out and posts FlutterEngineOnVsync onto the engine's strand.
class DrmDumbSink final : public ISurfaceSink {
 public:
  // Pixel format of the allocated dumb buffers. Chosen at Create()
  // and immutable thereafter; both ping-pong buffers always agree.
  enum class Format : uint8_t {
    kXRGB8888,  // 32 bpp, DRM_FORMAT_XRGB8888 — default.
    kRGB565,    // 16 bpp, DRM_FORMAT_RGB565   — bandwidth-saving.
  };

  // @p device_path is something like "/dev/dri/card0"; can be empty
  // to let the sink probe the first card found via the legacy
  // drmOpen lookup. Returns nullptr if no usable connector / CRTC /
  // mode is found.
  static std::unique_ptr<DrmDumbSink> Create(const std::string& device_path);

  ~DrmDumbSink() override;

  DrmDumbSink(const DrmDumbSink&) = delete;
  DrmDumbSink& operator=(const DrmDumbSink&) = delete;

  // ISurfaceSink — frame producer side (rasterizer thread).
  bool Present(const void* allocation,
               size_t row_bytes,
               size_t height) override;
  void OnSize(uint32_t width, uint32_t height) override;

  // ISurfaceSink — vsync wiring.
  [[nodiscard]] bool SupportsVsync() const override { return true; }
  void SubmitBaton(void* engine, intptr_t baton) override;
  void SetEngineHandle(void* engine) override;
  void SetPlatformTaskRunner(TaskRunner* runner) override;
  void StopVsyncMonitor() override;

  // For the SoftwareDisplay constructor, so it can report the actual
  // mode's refresh rate rather than a guessed default.
  [[nodiscard]] uint32_t mode_width() const { return mode_width_; }
  [[nodiscard]] uint32_t mode_height() const { return mode_height_; }
  [[nodiscard]] double refresh_rate_hz() const { return refresh_rate_hz_; }

 private:
  DrmDumbSink();

  // Probe the drm fd for a connector / CRTC / mode triple. Allocates
  // both dumb buffers and attaches buffer 0 to the CRTC. Returns false
  // on any failure; caller logs and falls back.
  bool InitDevice(const std::string& device_path);
  bool AllocBuffer(size_t index);
  void FreeBuffer(size_t index);

  // Walk the plane list and confirm at least one plane usable on
  // crtc_id_ advertises @p fourcc. Called from InitDevice to gate
  // the RGB565 path; XRGB8888 is assumed universally supported and
  // skips the probe.
  bool PlaneSupportsFormat(uint32_t fourcc) const;

  // Pack one frame's bytes from Flutter's BGRA-in-memory source into
  // the back buffer's allocated format. Handles row stride mismatch
  // + size clipping when the view doesn't match the mode exactly.
  void SwizzleInto(size_t buffer_index,
                   const void* allocation,
                   size_t src_row_bytes,
                   size_t src_height);

  // Static C trampoline for drmEventContext.page_flip_handler.
  static void PageFlipHandler(int fd,
                              unsigned int sequence,
                              unsigned int tv_sec,
                              unsigned int tv_usec,
                              void* user_data);

  void ArmFlipRead();
  void OnPageFlip(uint64_t tv_ns);

  // PostOnVsync mirrors drm_kms_egl's strand-marshalled
  // FlutterEngineOnVsync. now/period in CLOCK_MONOTONIC ns.
  void PostOnVsync(void* engine, intptr_t baton, uint64_t now_ns) const;

  int drm_fd_{-1};
  uint32_t connector_id_{0};
  uint32_t crtc_id_{0};
  uint32_t saved_crtc_id_{0};  // for restoring on shutdown
  std::optional<uint32_t> saved_fb_;
  uint32_t mode_width_{0};
  uint32_t mode_height_{0};
  double refresh_rate_hz_{60.0};
  Format format_{Format::kXRGB8888};

  // Page-flip event delivery. asio descriptor lives on the platform
  // task runner's io_context; ArmFlipRead schedules a one-shot read,
  // PageFlipHandler is the libdrm callback, OnPageFlip wraps it.
  std::optional<asio::posix::stream_descriptor> flip_descriptor_;

  // Two dumb buffers used in a strict front/back ping-pong driven by
  // page-flip completion (we always present the buffer that isn't
  // currently scanning out).
  struct Buffer {
    uint32_t handle{0};
    uint32_t pitch{0};
    uint32_t fb_id{0};
    uint64_t size{0};
    uint8_t* map{nullptr};
  };
  std::array<Buffer, 2> buffers_{};
  // Index of the buffer currently scanning out (or being flipped to);
  // the rasterizer renders into 1 - front_buffer_.
  size_t front_buffer_{0};
  // True between drmModePageFlip and the corresponding flip event.
  std::atomic<bool> flip_pending_{false};

  // Vsync baton plumbing — mirrors drm_kms_egl exactly.
  std::atomic<intptr_t> vsync_baton_{0};
  std::atomic<void*> engine_handle_{nullptr};
  std::atomic<TaskRunner*> platform_task_runner_{nullptr};
  // Refresh period as nanoseconds; computed once from the picked mode.
  std::atomic<uint64_t> refresh_period_ns_{16'666'667};

  // Tear-down latch.
  std::atomic<bool> stopped_{false};
};
