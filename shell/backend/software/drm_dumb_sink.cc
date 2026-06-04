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

#include "backend/software/drm_dumb_sink.h"
#include "logging/logging.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <utility>

#include <asio/post.hpp>

#include "backend/software/pixel_swizzle.h"
#include "libflutter_engine.h"
#include "logging.h"
#include "task_runner.h"

namespace {

// Sample the IVI_SW_DRM_FORMAT env var once at Create() time. Default
// (unset / empty / unrecognized) is kXRGB8888 — bit-for-bit identical
// to the pre-RGB565 behaviour. Unrecognized values warn so a CI typo
// doesn't silently land the operator on a different format.
DrmDumbSink::Format RequestedFormatFromEnv() {
  const char* env = std::getenv("IVI_SW_DRM_FORMAT");
  if (env == nullptr || env[0] == '\0') {
    return DrmDumbSink::Format::kXRGB8888;
  }
  const std::string_view v{env};
  if (v == "xrgb8888" || v == "xrgb" || v == "XRGB8888") {
    return DrmDumbSink::Format::kXRGB8888;
  }
  if (v == "rgb565" || v == "565" || v == "RGB565") {
    return DrmDumbSink::Format::kRGB565;
  }
  spdlog::warn(
      "[DrmDumbSink] IVI_SW_DRM_FORMAT='{}' unrecognized; "
      "using xrgb8888. Accepted: xrgb8888 (default), rgb565.",
      env);
  return DrmDumbSink::Format::kXRGB8888;
}

const char* FormatName(DrmDumbSink::Format f) {
  switch (f) {
    case DrmDumbSink::Format::kXRGB8888:
      return "xrgb8888";
    case DrmDumbSink::Format::kRGB565:
      return "rgb565";
  }
  return "?";
}

uint32_t FormatFourcc(DrmDumbSink::Format f) {
  switch (f) {
    case DrmDumbSink::Format::kXRGB8888:
      return DRM_FORMAT_XRGB8888;
    case DrmDumbSink::Format::kRGB565:
      return DRM_FORMAT_RGB565;
  }
  return DRM_FORMAT_XRGB8888;
}

uint32_t FormatBpp(DrmDumbSink::Format f) {
  switch (f) {
    case DrmDumbSink::Format::kXRGB8888:
      return 32;
    case DrmDumbSink::Format::kRGB565:
      return 16;
  }
  return 32;
}

bool DitherRequestedFromEnv() {
  const char* env = std::getenv("IVI_SW_DRM_DITHER");
  return env != nullptr && std::string_view(env) == "1";
}

}  // namespace

std::unique_ptr<DrmDumbSink> DrmDumbSink::Create(
    const std::string& device_path) {
  std::unique_ptr<DrmDumbSink> sink(new DrmDumbSink());
  sink->format_ = RequestedFormatFromEnv();
  sink->dither_ = DitherRequestedFromEnv();
  if (!sink->InitDevice(device_path)) {
    return nullptr;
  }
  return sink;
}

DrmDumbSink::DrmDumbSink() = default;

DrmDumbSink::~DrmDumbSink() {
  // Lifetime contract (mirrors the other backends): the embedder
  // must quiesce the rasterizer thread (FlutterEngineDeinitialize +
  // Shutdown, which joins raster) BEFORE dropping the sink. No
  // explicit synchronization here against an in-flight Present —
  // doing it correctly would require a mutex held across every
  // ioctl, which fights the lock-free hot path. FlutterView's dtor
  // calls StopVsyncMonitor before m_flutter_engine destructs, which
  // joins the rasterizer; by the time we get here Present is no
  // longer firing.
  StopVsyncMonitor();
  if (drm_fd_ >= 0) {
    // Restore the prior CRTC mapping if we modeset; without this the
    // console doesn't come back after the embedder exits on a bare
    // TTY. saved_fb_ being unset means we never modeset, so skip.
    if (saved_fb_.has_value() && saved_crtc_id_ != 0) {
      drmModeSetCrtc(drm_fd_, saved_crtc_id_, *saved_fb_, 0, 0, &connector_id_,
                     1, nullptr);
    }
    for (size_t i = 0; i < buffers_.size(); ++i) {
      FreeBuffer(i);
    }
    ::close(drm_fd_);
    drm_fd_ = -1;
  }
}

bool DrmDumbSink::InitDevice(const std::string& device_path) {
  // Open the modesetting node. Caller's responsibility to have
  // read+write access (typically `drm` group on systemd distros).
  const std::string path = device_path.empty() ? "/dev/dri/card0" : device_path;
  drm_fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (drm_fd_ < 0) {
    spdlog::error("[DrmDumbSink] open('{}'): {}", path, std::strerror(errno));
    return false;
  }
  spdlog::info("[DrmDumbSink] opened {}", path);

  drmModeRes* res = drmModeGetResources(drm_fd_);
  if (res == nullptr) {
    spdlog::error("[DrmDumbSink] drmModeGetResources: {}",
                  std::strerror(errno));
    return false;
  }

  // Pick the first connected connector with at least one mode.
  drmModeConnector* connector = nullptr;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c = drmModeGetConnector(drm_fd_, res->connectors[i]);
    if (c != nullptr && c->connection == DRM_MODE_CONNECTED &&
        c->count_modes > 0) {
      connector = c;
      break;
    }
    if (c != nullptr) {
      drmModeFreeConnector(c);
    }
  }
  if (connector == nullptr) {
    spdlog::error("[DrmDumbSink] no connected connector with modes");
    drmModeFreeResources(res);
    return false;
  }
  connector_id_ = connector->connector_id;

  // Find the kernel-flagged preferred mode (the default pick, as before).
  int preferred_idx = -1;
  for (int i = 0; i < connector->count_modes; ++i) {
    if ((connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0) {
      preferred_idx = i;
      break;
    }
  }
  int chosen_idx = preferred_idx >= 0 ? preferred_idx : 0;
  bool mode_from_env = false;

  // IVI_SW_DRM_MODE="<W>x<H>[@<R>]" selects a specific mode (refresh matched
  // against drmModeModeInfo::vrefresh in integer Hz when the @<R> is given). No
  // match → fall back to the preferred mode. Mirrors drm-kms-egl's --drm-mode.
  if (const char* spec = std::getenv("IVI_SW_DRM_MODE");
      spec != nullptr && spec[0] != '\0') {
    unsigned want_w = 0, want_h = 0, want_r = 0;
    const int n = std::sscanf(spec, "%ux%u@%u", &want_w, &want_h, &want_r);
    if (n >= 2) {
      int match = -1;
      for (int i = 0; i < connector->count_modes; ++i) {
        const auto& m = connector->modes[i];
        if (m.hdisplay == want_w && m.vdisplay == want_h &&
            (n < 3 || static_cast<unsigned>(m.vrefresh) == want_r)) {
          match = i;
          break;
        }
      }
      if (match >= 0) {
        chosen_idx = match;
        mode_from_env = true;
      } else {
        spdlog::warn(
            "[DrmDumbSink] IVI_SW_DRM_MODE='{}' not found on connector {}; "
            "using the {} mode",
            spec, connector->connector_id,
            preferred_idx >= 0 ? "preferred" : "first");
      }
    } else {
      spdlog::warn(
          "[DrmDumbSink] IVI_SW_DRM_MODE='{}' is not <W>x<H>[@<R>]; ignoring",
          spec);
    }
  }

  // Mode-list log (mirrors drm-kms-egl --drm-list-modes): the preferred mode is
  // flagged, the chosen mode gets a trailing '*'. Lets the operator see valid
  // IVI_SW_DRM_MODE values.
  for (int i = 0; i < connector->count_modes; ++i) {
    const auto& m = connector->modes[i];
    spdlog::info("[DrmDumbSink]   mode[{}] {}x{}@{}Hz{}{}", i, m.hdisplay,
                 m.vdisplay, m.vrefresh,
                 i == preferred_idx ? " (preferred)" : "",
                 i == chosen_idx ? " *" : "");
  }

  const drmModeModeInfo mode = connector->modes[chosen_idx];
  mode_width_ = mode.hdisplay;
  mode_height_ = mode.vdisplay;
  refresh_rate_hz_ =
      mode.vrefresh > 0 ? static_cast<double>(mode.vrefresh) : 60.0;
  refresh_period_ns_.store(static_cast<uint64_t>(1e9 / refresh_rate_hz_),
                           std::memory_order_release);
  spdlog::info("[DrmDumbSink] connector={} mode={}x{}@{:.2f}Hz{}",
               connector->connector_id, mode_width_, mode_height_,
               refresh_rate_hz_, mode_from_env ? " (IVI_SW_DRM_MODE)" : "");

  // Pick a CRTC: prefer the connector's currently-bound encoder/CRTC
  // to avoid disturbing the existing console binding.
  uint32_t crtc_id = 0;
  if (connector->encoder_id != 0) {
    drmModeEncoder* enc = drmModeGetEncoder(drm_fd_, connector->encoder_id);
    if (enc != nullptr) {
      if (enc->crtc_id != 0) {
        crtc_id = enc->crtc_id;
      }
      drmModeFreeEncoder(enc);
    }
  }
  // Fallback: walk connector's possible_crtcs bitmask.
  if (crtc_id == 0) {
    for (int i = 0; i < connector->count_encoders && crtc_id == 0; ++i) {
      drmModeEncoder* enc = drmModeGetEncoder(drm_fd_, connector->encoders[i]);
      if (enc == nullptr) {
        continue;
      }
      for (int c = 0; c < res->count_crtcs; ++c) {
        if ((enc->possible_crtcs & (1U << c)) != 0) {
          crtc_id = res->crtcs[c];
          break;
        }
      }
      drmModeFreeEncoder(enc);
    }
  }
  drmModeFreeConnector(connector);
  drmModeFreeResources(res);
  if (crtc_id == 0) {
    spdlog::error("[DrmDumbSink] no usable CRTC for connector");
    return false;
  }
  crtc_id_ = crtc_id;

  // Snapshot the existing CRTC binding so dtor can restore the
  // console. The fb id may be 0 if nothing was previously displayed.
  if (drmModeCrtc* prev = drmModeGetCrtc(drm_fd_, crtc_id_)) {
    saved_crtc_id_ = prev->crtc_id;
    saved_fb_ = prev->buffer_id;
    drmModeFreeCrtc(prev);
  }

  // Format negotiation. XRGB8888 is assumed universally supported
  // (every modern DRM driver advertises it on a primary plane); for
  // RGB565 we walk the CRTC's planes and confirm at least one of
  // them lists the fourcc, falling back to XRGB with a warn if not.
  // Caller's IVI_SW_DRM_FORMAT request lives in format_ already.
  if (format_ == Format::kRGB565 && !PlaneSupportsFormat(DRM_FORMAT_RGB565)) {
    spdlog::warn(
        "[DrmDumbSink] DRM_FORMAT_RGB565 not advertised by any plane "
        "usable on CRTC {}, falling back to XRGB8888",
        crtc_id_);
    format_ = Format::kXRGB8888;
  }
  spdlog::info("[DrmDumbSink] format={} ({} bpp){}", FormatName(format_),
               FormatBpp(format_),
               (format_ == Format::kRGB565 && dither_) ? " +bayer-dither" : "");

  // Allocate both dumb buffers at the mode's dimensions.
  for (size_t i = 0; i < buffers_.size(); ++i) {
    if (!AllocBuffer(i)) {
      return false;
    }
  }

  // Modeset onto buffer 0 so the panel lights up immediately. Frames
  // arrive via Present() into buffer 1, with page-flip swapping front.
  drmModeModeInfo modeset_mode = mode;
  if (drmModeSetCrtc(drm_fd_, crtc_id_, buffers_[0].fb_id, 0, 0, &connector_id_,
                     1, &modeset_mode) != 0) {
    spdlog::error("[DrmDumbSink] drmModeSetCrtc: {}", std::strerror(errno));
    return false;
  }
  front_buffer_ = 0;
  return true;
}

bool DrmDumbSink::AllocBuffer(const size_t index) {
  drm_mode_create_dumb create{};
  create.width = mode_width_;
  create.height = mode_height_;
  create.bpp = FormatBpp(format_);
  if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
    spdlog::error("[DrmDumbSink] DRM_IOCTL_MODE_CREATE_DUMB: {}",
                  std::strerror(errno));
    return false;
  }
  Buffer& b = buffers_[index];
  b.handle = create.handle;
  b.pitch = create.pitch;
  b.size = create.size;

  // Attach as an FB. fourcc layout is endian-invariant per drm_fourcc.h:
  // XRGB8888 → bytes [B,G,R,X], RGB565 → LE 16-bit word [R5 G6 B5].
  const uint32_t handles[4] = {b.handle, 0, 0, 0};
  const uint32_t pitches[4] = {b.pitch, 0, 0, 0};
  const uint32_t offsets[4] = {0, 0, 0, 0};
  if (drmModeAddFB2(drm_fd_, mode_width_, mode_height_, FormatFourcc(format_),
                    handles, pitches, offsets, &b.fb_id, 0) != 0) {
    spdlog::error("[DrmDumbSink] drmModeAddFB2({}): {}", FormatName(format_),
                  std::strerror(errno));
    return false;
  }

  // mmap so the rasterizer thread can memcpy into it. Dumb buffers
  // are cached on most kernels; that's fine for one writer.
  drm_mode_map_dumb map_req{};
  map_req.handle = b.handle;
  if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
    spdlog::error("[DrmDumbSink] DRM_IOCTL_MODE_MAP_DUMB: {}",
                  std::strerror(errno));
    return false;
  }
  void* mapped = ::mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        drm_fd_, static_cast<off_t>(map_req.offset));
  if (mapped == MAP_FAILED) {
    spdlog::error("[DrmDumbSink] mmap: {}", std::strerror(errno));
    return false;
  }
  b.map = static_cast<uint8_t*>(mapped);
  return true;
}

void DrmDumbSink::FreeBuffer(const size_t index) {
  Buffer& b = buffers_[index];
  if (b.map != nullptr) {
    ::munmap(b.map, b.size);
    b.map = nullptr;
  }
  if (b.fb_id != 0) {
    drmModeRmFB(drm_fd_, b.fb_id);
    b.fb_id = 0;
  }
  if (b.handle != 0) {
    drm_mode_destroy_dumb destroy{};
    destroy.handle = b.handle;
    drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    b.handle = 0;
  }
}

bool DrmDumbSink::PlaneSupportsFormat(const uint32_t fourcc) const {
  if (drm_fd_ < 0 || crtc_id_ == 0) {
    return false;
  }
  // Universal-planes cap exposes the primary plane in addition to
  // overlays; without it some drivers only return overlays from
  // drmModeGetPlaneResources. Failures here are non-fatal — older
  // kernels may not implement the cap but still return primary planes.
  drmSetClientCap(drm_fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  // Find the CRTC index for crtc_id_; possible_crtcs is a bitmask
  // indexed against drmModeRes::crtcs[], not raw crtc ids.
  uint32_t crtc_index = UINT32_MAX;
  if (drmModeRes* res = drmModeGetResources(drm_fd_)) {
    for (int i = 0; i < res->count_crtcs; ++i) {
      if (res->crtcs[i] == crtc_id_) {
        crtc_index = static_cast<uint32_t>(i);
        break;
      }
    }
    drmModeFreeResources(res);
  }
  if (crtc_index == UINT32_MAX) {
    return false;
  }

  drmModePlaneRes* plane_res = drmModeGetPlaneResources(drm_fd_);
  if (plane_res == nullptr) {
    return false;
  }
  bool found = false;
  for (uint32_t i = 0; i < plane_res->count_planes && !found; ++i) {
    drmModePlane* p = drmModeGetPlane(drm_fd_, plane_res->planes[i]);
    if (p == nullptr) {
      continue;
    }
    if ((p->possible_crtcs & (1U << crtc_index)) != 0) {
      for (uint32_t f = 0; f < p->count_formats && !found; ++f) {
        if (p->formats[f] == fourcc) {
          found = true;
        }
      }
    }
    drmModeFreePlane(p);
  }
  drmModeFreePlaneResources(plane_res);
  return found;
}

void DrmDumbSink::OnSize(uint32_t /*width*/, uint32_t /*height*/) {
  // Mode is fixed at construction; the swizzle path clips/pads if
  // Flutter's view geometry doesn't match. No-op here.
}

void DrmDumbSink::SwizzleInto(const size_t buffer_index,
                              const void* allocation,
                              const size_t src_row_bytes,
                              const size_t src_height) {
  Buffer& b = buffers_[buffer_index];
  if (b.map == nullptr) {
    return;
  }
  const auto* src = static_cast<const uint8_t*>(allocation);
  const size_t src_width_px = src_row_bytes / 4;
  const size_t copy_width_px = std::min<size_t>(src_width_px, mode_width_);
  const size_t copy_height = std::min<size_t>(src_height, mode_height_);

  // Pixel-format dispatch. XRGB8888 → 4 B/px, FlutterToBGRX8888
  // (memcpy + alpha-force on LE). RGB565 → 2 B/px, FlutterToRGB565
  // (NEON vld4 + widening-shift + sri on ARM, scalar elsewhere);
  // dither_ adds a Bayer 4×4 offset before quantization to hide
  // gradient banding at the cost of bit-exact goldens.
  if (format_ == Format::kRGB565) {
    for (size_t y = 0; y < copy_height; ++y) {
      const uint8_t* src_row = src + y * src_row_bytes;
      auto* dst_row = reinterpret_cast<uint16_t*>(b.map + y * b.pitch);
      if (dither_) {
        ivi::swizzle::FlutterToRGB565_BayerDither(dst_row, src_row,
                                                  copy_width_px, y);
      } else {
        ivi::swizzle::FlutterToRGB565(dst_row, src_row, copy_width_px);
      }
      if (copy_width_px < mode_width_) {
        std::memset(dst_row + copy_width_px, 0,
                    (mode_width_ - copy_width_px) * 2);
      }
    }
    for (size_t y = copy_height; y < mode_height_; ++y) {
      std::memset(b.map + y * b.pitch, 0, mode_width_ * 2);
    }
  } else {
    for (size_t y = 0; y < copy_height; ++y) {
      const uint8_t* src_row = src + y * src_row_bytes;
      uint8_t* dst_row = b.map + y * b.pitch;
      ivi::swizzle::FlutterToBGRX8888(dst_row, src_row, copy_width_px);
      if (copy_width_px < mode_width_) {
        std::memset(dst_row + copy_width_px * 4, 0,
                    (mode_width_ - copy_width_px) * 4);
      }
    }
    for (size_t y = copy_height; y < mode_height_; ++y) {
      std::memset(b.map + y * b.pitch, 0, mode_width_ * 4);
    }
  }
}

bool DrmDumbSink::Present(const void* allocation,
                          const size_t row_bytes,
                          const size_t height) {
  if (stopped_.load(std::memory_order_acquire)) {
    return true;
  }
  // Strict ping-pong: render into the buffer that isn't currently
  // displayed. Wait briefly if a previous flip hasn't completed yet.
  // Bounded by 5× the refresh period — if the kernel drops a
  // PAGE_FLIP_EVENT (driver hang / suspend storm) we force-clear the
  // flag and warn rather than parking the rasterizer forever.
  using namespace std::chrono_literals;
  const uint64_t period_ns = refresh_period_ns_.load(std::memory_order_acquire);
  const uint64_t deadline_ns = (period_ns == 0 ? 16'666'667ULL : period_ns) * 5;
  const auto wait_start = std::chrono::steady_clock::now();
  while (flip_pending_.load(std::memory_order_acquire)) {
    if (stopped_.load(std::memory_order_acquire)) {
      return true;
    }
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - wait_start)
            .count();
    if (static_cast<uint64_t>(elapsed_ns) > deadline_ns) {
      spdlog::warn(
          "[DrmDumbSink] PAGE_FLIP_EVENT not received within {} ns; "
          "force-clearing flip_pending_ and continuing",
          deadline_ns);
      flip_pending_.store(false, std::memory_order_release);
      break;
    }
    std::this_thread::sleep_for(500us);
  }

  // Reject the flip if the platform task runner hasn't armed our
  // event descriptor yet: queuing PAGE_FLIP_EVENT with no reader
  // would leave flip_pending_ stuck. The first Present after
  // SetPlatformTaskRunner picks up where we left off. Under
  // FlutterView's normal init order this branch never fires (Engine
  // ::Run is what triggers Present, and SetPlatformTaskRunner is
  // called immediately after Engine::Run returns), but the guard
  // protects against future re-orderings.
  if (!flip_descriptor_.has_value()) {
    spdlog::warn(
        "[DrmDumbSink] Present before SetPlatformTaskRunner armed flip "
        "descriptor; skipping page-flip for this frame");
    return true;
  }

  const size_t back = 1 - front_buffer_;
  SwizzleInto(back, allocation, row_bytes, height);

  // Schedule the page-flip. flip_pending_ goes high before the
  // ioctl so an out-of-band OnPageFlip can't observe the prior
  // state. Flutter's rasterizer is single-threaded so Present is
  // never re-entered concurrently.
  flip_pending_.store(true, std::memory_order_release);
  if (drmModePageFlip(drm_fd_, crtc_id_, buffers_[back].fb_id,
                      DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
    spdlog::warn("[DrmDumbSink] drmModePageFlip: {}", std::strerror(errno));
    flip_pending_.store(false, std::memory_order_release);

    // Drain the parked baton (if any) inline so Flutter doesn't sit
    // waiting on an OnVsync from a flip that never queued. No real
    // vblank timestamp here — use the engine clock so frame_target_
    // time math has a usable seed.
    const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
    if (baton != 0) {
      void* engine = engine_handle_.load(std::memory_order_acquire);
      if (engine != nullptr) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t now_ns =
            static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
            static_cast<uint64_t>(ts.tv_nsec);
        PostOnVsync(engine, baton, now_ns);
      }
    }
    return false;
  }
  front_buffer_ = back;
  return true;
}

void DrmDumbSink::SetEngineHandle(void* engine) {
  engine_handle_.store(engine, std::memory_order_release);
}

void DrmDumbSink::SetPlatformTaskRunner(TaskRunner* runner) {
  platform_task_runner_.store(runner, std::memory_order_release);
  if (runner == nullptr || runner->GetIoContext() == nullptr || drm_fd_ < 0) {
    return;
  }
  // Re-entry guard. flip_descriptor_'s 2-arg emplace adopts drm_fd_;
  // a second emplace without prior release()/reset() would destroy
  // the existing descriptor and close drm_fd_ underneath us. In the
  // current FlutterView lifecycle SetPlatformTaskRunner is called
  // exactly once, but defending against future plumbing changes is
  // cheap.
  if (flip_descriptor_.has_value()) {
    spdlog::warn(
        "[DrmDumbSink] SetPlatformTaskRunner called while flip descriptor "
        "already armed; ignoring");
    return;
  }
  // Adopt the drm fd into an asio stream_descriptor on the platform
  // task runner's io_context, so page-flip events drain on the same
  // thread that runs FlutterEngineOnVsync.
  flip_descriptor_.emplace(*runner->GetIoContext(), drm_fd_);
  ArmFlipRead();
}

void DrmDumbSink::ArmFlipRead() {
  if (!flip_descriptor_.has_value() ||
      stopped_.load(std::memory_order_acquire)) {
    return;
  }
  flip_descriptor_->async_wait(
      asio::posix::stream_descriptor::wait_read,
      [this](const std::error_code& ec) {
        if (ec) {
          if (ec != asio::error::operation_aborted) {
            spdlog::warn("[DrmDumbSink] flip monitor wait: {}", ec.message());
          }
          return;
        }
        if (drm_fd_ < 0 || stopped_.load(std::memory_order_acquire)) {
          return;
        }
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = &DrmDumbSink::PageFlipHandler;
        drmHandleEvent(drm_fd_, &ctx);
        ArmFlipRead();  // re-arm for the next vblank
      });
}

void DrmDumbSink::PageFlipHandler(int /*fd*/,
                                  unsigned int /*sequence*/,
                                  const unsigned int tv_sec,
                                  const unsigned int tv_usec,
                                  void* user_data) {
  auto* self = static_cast<DrmDumbSink*>(user_data);
  if (self == nullptr) {
    return;
  }
  const uint64_t tv_ns = static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL +
                         static_cast<uint64_t>(tv_usec) * 1000ULL;
  self->OnPageFlip(tv_ns);
}

void DrmDumbSink::OnPageFlip(const uint64_t tv_ns) {
  // Thread contract: this fires from the asio handler armed against
  // the platform task runner's io_context, so it runs on the same
  // thread the runner uses for FlutterEngineOnVsync. PostOnVsync's
  // asio::post into the runner's strand is therefore a same-thread
  // forward and never races with runner destruction (the runner is
  // joined by FlutterEngine teardown, which precedes this sink's
  // dtor per the lifetime contract).
  flip_pending_.store(false, std::memory_order_release);
  // Drain the parked baton, if any, and hand it back with the
  // kernel-provided scanout timestamp.
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
  void* engine = engine_handle_.load(std::memory_order_acquire);
  if (engine == nullptr) {
    return;
  }
  PostOnVsync(engine, baton, tv_ns);
}

void DrmDumbSink::SubmitBaton(void* engine, const intptr_t baton) {
  engine_handle_.store(engine, std::memory_order_release);
  vsync_baton_.store(baton, std::memory_order_release);

  // Idle-kick: if no flip is in flight (first frame, post-resume),
  // drain the baton inline. Otherwise the next page-flip event picks
  // it up.
  if (flip_pending_.load(std::memory_order_acquire)) {
    return;
  }
  if (const intptr_t mine = vsync_baton_.exchange(0, std::memory_order_acq_rel);
      mine != 0) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now_ns =
        static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
        static_cast<uint64_t>(ts.tv_nsec);
    PostOnVsync(engine, mine, now_ns);
  }
}

void DrmDumbSink::PostOnVsync(void* engine,
                              const intptr_t baton,
                              const uint64_t now_ns) const {
  if (engine == nullptr || baton == 0) {
    return;
  }
  auto* runner = platform_task_runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return;
  }
  const uint64_t period_ns = refresh_period_ns_.load(std::memory_order_acquire);
  // Flutter's OnVsync takes the strongly-typed engine handle; the
  // sink stored it as void* to keep the ISurfaceSink interface
  // header-only. Cast back at the call site.
  auto* engine_typed = static_cast<FLUTTER_API_SYMBOL(FlutterEngine)>(engine);
  asio::post(*runner->GetStrandContext(), [engine_typed, baton, now_ns,
                                           period_ns]() {
    LibFlutterEngine->OnVsync(engine_typed, baton, now_ns, now_ns + period_ns);
  });
}

void DrmDumbSink::StopVsyncMonitor() {
  if (stopped_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (flip_descriptor_.has_value()) {
    std::error_code ec;
    flip_descriptor_->cancel(ec);
    (void)flip_descriptor_->release();
    flip_descriptor_.reset();
  }
  platform_task_runner_.store(nullptr, std::memory_order_release);
  vsync_baton_.store(0, std::memory_order_release);
  engine_handle_.store(nullptr, std::memory_order_release);

  // Drain a lingering page-flip event synchronously — mirrors
  // drm_kms_egl's tear-down so destructors don't race with an
  // in-flight commit.
  if (drm_fd_ >= 0 && flip_pending_.load(std::memory_order_acquire)) {
    using namespace std::chrono_literals;
    constexpr int kDrainTimeoutMs = 100;
    constexpr int kPollSliceMs = 10;
    int elapsed_ms = 0;
    while (elapsed_ms < kDrainTimeoutMs &&
           flip_pending_.load(std::memory_order_acquire)) {
      pollfd pfd{drm_fd_, POLLIN, 0};
      const int rc = ::poll(&pfd, 1, kPollSliceMs);
      if (rc > 0 && (pfd.revents & POLLIN)) {
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = &DrmDumbSink::PageFlipHandler;
        drmHandleEvent(drm_fd_, &ctx);
      } else if (rc < 0 && errno != EINTR) {
        break;
      }
      elapsed_ms += kPollSliceMs;
    }
    if (flip_pending_.load(std::memory_order_acquire)) {
      spdlog::warn(
          "[DrmDumbSink] StopVsyncMonitor: flip never arrived; "
          "force-clearing flip_pending_");
      flip_pending_.store(false, std::memory_order_release);
    }
  }
}

namespace {

// List one card's driver + connectors/modes to stdout. Returns 0 on success,
// non-zero if the device can't be opened or has no DRM resources (e.g. a
// render-only node with no KMS).
int ListCardModes(const std::string& device_path) {
  const int fd = ::open(device_path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return 1;  // not present / no permission — silently skipped during a scan
  }
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr) {
    ::close(fd);  // render node (renderD*) or no KMS — skip
    return 1;
  }
  drmVersion* ver = drmGetVersion(fd);
  std::printf("%s  driver=%s, %d connector(s):\n", device_path.c_str(),
              (ver != nullptr && ver->name != nullptr) ? ver->name : "?",
              res->count_connectors);
  if (ver != nullptr) {
    drmFreeVersion(ver);
  }
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c == nullptr) {
      continue;
    }
    const bool connected = c->connection == DRM_MODE_CONNECTED;
    std::printf("  connector %u: %s, %d mode(s)\n", c->connector_id,
                connected ? "connected" : "disconnected", c->count_modes);
    for (int m = 0; m < c->count_modes; ++m) {
      const drmModeModeInfo& mi = c->modes[m];
      const bool pref = (mi.type & DRM_MODE_TYPE_PREFERRED) != 0;
      std::printf("    [%d] %ux%u@%uHz%s\n", m, mi.hdisplay, mi.vdisplay,
                  mi.vrefresh, pref ? "  (preferred)" : "");
    }
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  ::close(fd);
  return 0;
}

}  // namespace

int PrintDumbSinkModes(const std::string& device_path) {
  // An explicit device lists just that card; otherwise scan every
  // /dev/dri/card* so the operator can see which card to pass to --drm-device
  // (and which mode to IVI_SW_DRM_MODE) without hard-coding card0.
  if (!device_path.empty()) {
    if (ListCardModes(device_path) != 0) {
      spdlog::error("[DrmDumbSink] {} has no DRM/KMS resources", device_path);
      return 1;
    }
    return 0;
  }
  std::printf(
      "DRM cards (--drm-device /dev/dri/cardN selects one; IVI_SW_DRM_MODE "
      "picks a mode):\n");
  int listed = 0;
  for (int i = 0; i < 16; ++i) {
    const std::string dev = "/dev/dri/card" + std::to_string(i);
    if (::access(dev.c_str(), F_OK) != 0) {
      continue;
    }
    if (ListCardModes(dev) == 0) {
      ++listed;
    }
  }
  if (listed == 0) {
    spdlog::error("[DrmDumbSink] no KMS-capable /dev/dri/card* found");
    return 1;
  }
  return 0;
}
