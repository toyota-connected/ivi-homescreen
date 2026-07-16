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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include "backend/software/surface_sink.h"
#include "vsync/ivsync_provider.h"

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

  // wayland-leased-drm: drive an externally-owned DRM fd (the master fd from
  // wp_drm_lease_v1.lease_fd) instead of opening a card by path.
  //
  // Nothing has to be subtracted for a lease: this sink does no master handling
  // at all, and a lease fd arrives master over its object set -- strictly
  // better than the implicit-master assumption of the path above. The legacy
  // modeset and dumb-buffer ioctls it uses are valid on a lease fd, and because
  // the kernel filters the fd's resource view to the leased objects, the "first
  // connected connector with modes" pick lands on the leased connector by
  // construction.
  //
  // @p drm_fd is borrowed: the sink never closes it. @p fd_owner is the opaque
  // keep-alive that does own it (the LeaseHold) and must outlive the sink.
  // @p revoked, when set, is polled by Present(): once the lease is revoked its
  // KMS objects are gone from the fd's view and every commit naming them fails,
  // so frames are dropped rather than spraying ioctl errors. Null = never
  // gated.
  static std::unique_ptr<DrmDumbSink> Create(int drm_fd,
                                             std::shared_ptr<void> fd_owner,
                                             std::function<bool()> revoked);

  ~DrmDumbSink() override;

  DrmDumbSink(const DrmDumbSink&) = delete;
  DrmDumbSink& operator=(const DrmDumbSink&) = delete;

  // ISurfaceSink — frame producer side (rasterizer thread).
  bool Present(const void* allocation,
               size_t row_bytes,
               size_t height) override;
  void OnSize(uint32_t width, uint32_t height) override;
  // Composited onto each frame in Present (XRGB8888 path), after the swizzle
  // and before the page flip — no save/restore needed since the whole back
  // buffer is repacked every frame.
  void SetCursor(std::shared_ptr<SoftwareCursor> cursor) override;

  // ISurfaceSink — vsync wiring.
  [[nodiscard]] bool SupportsVsync() const override { return true; }
  void SubmitBaton(void* engine, intptr_t baton) override;
  void SetEngineHandle(void* engine) override;
  void SetPlatformTaskRunner(TaskRunner* runner) override;
  void StopVsyncMonitor() override;
  void SetMotionToPhoton(profiling::MotionToPhoton* m2p) override {
    m2p_ = m2p;
  }

  // For the SoftwareDisplay constructor, so it can report the actual
  // mode's refresh rate rather than a guessed default.
  [[nodiscard]] uint32_t mode_width() const { return mode_width_; }
  [[nodiscard]] uint32_t mode_height() const { return mode_height_; }
  [[nodiscard]] double refresh_rate_hz() const { return refresh_rate_hz_; }

  // The picked connector mode's extent — the SoftwareBackend adopts this as
  // the engine viewport so Flutter renders at the panel's native resolution.
  [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> NativeSize()
      const override {
    return std::make_pair(mode_width_, mode_height_);
  }

 private:
  DrmDumbSink();

  // Open @p device_path, then hand off to InitFromFd. Returns false on any
  // failure; caller logs and falls back.
  bool InitDevice(const std::string& device_path);

  // Probe drm_fd_ for a connector / CRTC / mode triple. Allocates both dumb
  // buffers and attaches buffer 0 to the CRTC. Split out from InitDevice at the
  // open() so the same probing runs against a supplied fd (a DRM lease), which
  // is the whole of the leased software tier.
  bool InitFromFd();
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

  int drm_fd_{-1};

  // False when drm_fd_ is a borrowed lease fd: the destructor must not close
  // what the LeaseHold owns and will close itself.
  bool owns_fd_{true};

  // Keeps the borrowed fd's owner alive for the sink's lifetime. Opaque so this
  // header stays free of the lease client -- the sink needs the fd to stay
  // valid, not to know what keeps it so. Null on the path-opened path.
  std::shared_ptr<void> fd_owner_;

  // Lease revocation gate polled by Present(); null on the path-opened path,
  // where there is no lease to lose.
  std::function<bool()> revoked_;

  uint32_t connector_id_{0};
  uint32_t crtc_id_{0};
  uint32_t saved_crtc_id_{0};  // for restoring on shutdown
  std::optional<uint32_t> saved_fb_;
  uint32_t mode_width_{0};
  uint32_t mode_height_{0};
  double refresh_rate_hz_{60.0};
  Format format_{Format::kXRGB8888};
  // IVI_SW_DRM_DITHER=1 enables Bayer 4×4 dithering on the RGB565
  // pack path (no-op for XRGB8888 — there's no precision loss to
  // hide). Default off so CI goldens stay bit-exact.
  bool dither_{false};

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

  // Shared baton machinery (park/drain/marshal + #210 cold-start safety). The
  // page-flip handler drives it: SetSourcePending() at the flip transitions and
  // DeliverVsync() on completion. engine_handle_/platform_task_runner_ are
  // cached only to re-pass both to vsync_.SetEngine() from the separate hooks.
  ivi::IVsyncProvider vsync_;
  std::atomic<void*> engine_handle_{nullptr};
  std::atomic<TaskRunner*> platform_task_runner_{nullptr};

  // Motion-to-photon profiler owned by SoftwareBackend, or nullptr when not
  // profiling. Set via SetMotionToPhoton; read on the platform-runner thread
  // (OnPageFlip) to record the scanout endpoint.
  profiling::MotionToPhoton* m2p_{nullptr};
  // Refresh period as nanoseconds; computed once from the picked mode.
  std::atomic<uint64_t> refresh_period_ns_{16'666'667};

  // Software cursor composited onto each frame (XRGB8888 path); shared with the
  // seat, which updates its position. Null when the cursor is disabled.
  std::shared_ptr<SoftwareCursor> cursor_;

  // Tear-down latch.
  std::atomic<bool> stopped_{false};
};

// Open @p device_path, enumerate every connector's modes (flagging connected /
// preferred), print to stdout, and return 0 (non-zero on open/query failure).
// The dumb-sink analogue of PrintDrmModes — backs --drm-list-modes on software
// builds so valid IVI_SW_DRM_MODE values can be discovered without launching
// the app onto the display.
int PrintDumbSinkModes(const std::string& device_path);
