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

#include "backend/software/fbdev_sink.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "backend/software/pixel_swizzle.h"
#include "logging.h"

std::unique_ptr<FbDevSink> FbDevSink::Create(const std::string& device_path) {
  std::unique_ptr<FbDevSink> sink(new FbDevSink());
  if (!sink->Init(device_path)) {
    return nullptr;
  }
  return sink;
}

FbDevSink::FbDevSink() = default;

FbDevSink::~FbDevSink() {
  if (fb_map_ != nullptr && fb_size_ != 0) {
    ::munmap(fb_map_, fb_size_);
    fb_map_ = nullptr;
  }
  if (fb_fd_ >= 0) {
    ::close(fb_fd_);
    fb_fd_ = -1;
  }
}

bool FbDevSink::Init(const std::string& device_path) {
  const std::string path = device_path.empty() ? "/dev/fb0" : device_path;
  fb_fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fb_fd_ < 0) {
    spdlog::error("[FbDevSink] open('{}'): {}", path, std::strerror(errno));
    return false;
  }

  fb_var_screeninfo var{};
  if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &var) != 0) {
    spdlog::error("[FbDevSink] FBIOGET_VSCREENINFO: {}", std::strerror(errno));
    return false;
  }
  fb_fix_screeninfo fix{};
  if (ioctl(fb_fd_, FBIOGET_FSCREENINFO, &fix) != 0) {
    spdlog::error("[FbDevSink] FBIOGET_FSCREENINFO: {}", std::strerror(errno));
    return false;
  }

  // We only support 32-bpp packed BGRA / BGRX truecolor layouts: red
  // at byte 2, green at byte 1, blue at byte 0 (little-endian DWORD
  // 0xAARRGGBB / 0x00RRGGBB). That covers the universal modern fbdev
  // surface and kernel `vfb` module's default. Other layouts (RGB565,
  // palettized, planar non-standard) would need a different swizzle
  // path; refuse loudly rather than corrupt the panel.
  // var.nonstd != 0 signals a driver-specific layout that doesn't
  // follow the standard red/green/blue offset+length scheme, so
  // gate on that too.
  const bool ok_format = var.bits_per_pixel == 32 && var.nonstd == 0 &&
                         var.red.offset == 16 && var.red.length == 8 &&
                         var.green.offset == 8 && var.green.length == 8 &&
                         var.blue.offset == 0 && var.blue.length == 8;
  if (!ok_format) {
    spdlog::error(
        "[FbDevSink] unsupported pixel format on '{}': bpp={}, nonstd={}, "
        "R[ofs={},len={}] G[ofs={},len={}] B[ofs={},len={}]. "
        "Need 32-bpp BGRA/BGRX (R@16, G@8, B@0, nonstd=0).",
        path, var.bits_per_pixel, var.nonstd, var.red.offset, var.red.length,
        var.green.offset, var.green.length, var.blue.offset, var.blue.length);
    return false;
  }

  fb_width_ = var.xres;
  fb_height_ = var.yres;
  fb_stride_ = fix.line_length;
  fb_size_ = fix.smem_len;
  // Cast to size_t before multiplying so a pathological driver
  // reporting an oversized xres can't overflow uint32_t and pass the
  // sanity gate. (On 64-bit Linux the cast is the real protection.)
  const size_t expected_row_bytes = static_cast<size_t>(fb_width_) * 4;
  const size_t expected_size =
      static_cast<size_t>(fb_stride_) * static_cast<size_t>(fb_height_);
  if (fb_size_ == 0 || fb_stride_ < expected_row_bytes ||
      fb_size_ < expected_size) {
    spdlog::error(
        "[FbDevSink] implausible framebuffer dims: {}x{} stride={} size={}",
        fb_width_, fb_height_, fb_stride_, fb_size_);
    return false;
  }

  void* mapped =
      ::mmap(nullptr, fb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
  if (mapped == MAP_FAILED) {
    spdlog::error("[FbDevSink] mmap: {}", std::strerror(errno));
    return false;
  }
  fb_map_ = static_cast<uint8_t*>(mapped);

  spdlog::info("[FbDevSink] opened {} ({}x{}, stride={}, smem_len={})", path,
               fb_width_, fb_height_, fb_stride_, fb_size_);
  return true;
}

bool FbDevSink::Present(const void* allocation,
                        const size_t row_bytes,
                        const size_t height) {
  if (fb_map_ == nullptr) {
    return false;
  }
  const auto* src = static_cast<const uint8_t*>(allocation);
  const size_t src_width_px = row_bytes / 4;
  const size_t copy_width_px = std::min<size_t>(src_width_px, fb_width_);
  const size_t copy_height = std::min<size_t>(height, fb_height_);

  // Init() validated fbdev as red.offset=16, green.offset=8,
  // blue.offset=0 — memory layout [B, G, R, X] on LE matches
  // Flutter's BGRA source exactly, so FlutterToBGRX8888 collapses to
  // memcpy + alpha-fix. On BE it byte-swaps.
  for (size_t y = 0; y < copy_height; ++y) {
    const uint8_t* src_row = src + y * row_bytes;
    uint8_t* dst_row = fb_map_ + y * fb_stride_;
    ivi::swizzle::FlutterToBGRX8888(dst_row, src_row, copy_width_px);
    if (copy_width_px < fb_width_) {
      std::memset(dst_row + copy_width_px * 4, 0,
                  (fb_width_ - copy_width_px) * 4);
    }
  }
  // Pad rows below the view for the same reason.
  for (size_t y = copy_height; y < fb_height_; ++y) {
    std::memset(fb_map_ + y * fb_stride_, 0, fb_width_ * 4);
  }
  return true;
}
