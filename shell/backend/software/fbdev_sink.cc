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
#include "logging/logging.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>

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
    ihs::log::error("[FbDevSink] open('{}'): {}", path, std::strerror(errno));
    return false;
  }

  fb_var_screeninfo var{};
  if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &var) != 0) {
    ihs::log::error("[FbDevSink] FBIOGET_VSCREENINFO: {}",
                    std::strerror(errno));
    return false;
  }
  fb_fix_screeninfo fix{};
  if (ioctl(fb_fd_, FBIOGET_FSCREENINFO, &fix) != 0) {
    ihs::log::error("[FbDevSink] FBIOGET_FSCREENINFO: {}",
                    std::strerror(errno));
    return false;
  }

  // Two accepted layouts, picked off FBIOGET_VSCREENINFO. var.nonstd
  // != 0 signals a driver-specific layout that doesn't follow the
  // standard red/green/blue offset+length scheme, so gate on that
  // for both.
  //
  // BGRX8888: 32-bpp packed truecolor, red at byte 2 / green byte 1
  // / blue byte 0 (LE DWORD 0xAARRGGBB / 0x00RRGGBB). The universal
  // modern fbdev surface and kernel `vfb` default.
  //
  // RGB565: 16-bpp packed, red in bits 15..11 / green 10..5 / blue
  // 4..0 of the LE 16-bit word. Common on cost-sensitive panels and
  // kernel `vfb` configured with `-16`.
  const bool ok_bgrx32 = var.bits_per_pixel == 32 && var.nonstd == 0 &&
                         var.red.offset == 16 && var.red.length == 8 &&
                         var.green.offset == 8 && var.green.length == 8 &&
                         var.blue.offset == 0 && var.blue.length == 8;
  const bool ok_rgb565 = var.bits_per_pixel == 16 && var.nonstd == 0 &&
                         var.red.offset == 11 && var.red.length == 5 &&
                         var.green.offset == 5 && var.green.length == 6 &&
                         var.blue.offset == 0 && var.blue.length == 5;
  if (!ok_bgrx32 && !ok_rgb565) {
    ihs::log::error(
        "[FbDevSink] unsupported pixel format on '{}': bpp={}, nonstd={}, "
        "R[ofs={},len={}] G[ofs={},len={}] B[ofs={},len={}]. "
        "Need 32-bpp BGRA/BGRX (R@16, G@8, B@0, nonstd=0) or 16-bpp "
        "RGB565 (R@11/5, G@5/6, B@0/5, nonstd=0).",
        path, var.bits_per_pixel, var.nonstd, var.red.offset, var.red.length,
        var.green.offset, var.green.length, var.blue.offset, var.blue.length);
    return false;
  }
  format_ = ok_rgb565 ? Format::kRGB565 : Format::kBGRX8888;
  if (const char* env = std::getenv("IVI_SW_DRM_DITHER");
      env != nullptr && std::string_view(env) == "1") {
    dither_ = true;
  }

  fb_width_ = var.xres;
  fb_height_ = var.yres;
  fb_stride_ = fix.line_length;
  fb_size_ = fix.smem_len;
  // Cast to size_t before multiplying so a pathological driver
  // reporting an oversized xres can't overflow uint32_t and pass the
  // sanity gate. (On 64-bit Linux the cast is the real protection.)
  const size_t bpp_bytes = format_ == Format::kRGB565 ? 2 : 4;
  const size_t expected_row_bytes = static_cast<size_t>(fb_width_) * bpp_bytes;
  const size_t expected_size =
      static_cast<size_t>(fb_stride_) * static_cast<size_t>(fb_height_);
  if (fb_size_ == 0 || fb_stride_ < expected_row_bytes ||
      fb_size_ < expected_size) {
    ihs::log::error(
        "[FbDevSink] implausible framebuffer dims: {}x{} stride={} size={}",
        fb_width_, fb_height_, fb_stride_, fb_size_);
    return false;
  }

  void* mapped =
      ::mmap(nullptr, fb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
  if (mapped == MAP_FAILED) {
    ihs::log::error("[FbDevSink] mmap: {}", std::strerror(errno));
    return false;
  }
  fb_map_ = static_cast<uint8_t*>(mapped);

  ihs::log::info(
      "[FbDevSink] opened {} ({}x{}, format={}{}, stride={}, smem_len={})",
      path, fb_width_, fb_height_,
      format_ == Format::kRGB565 ? "rgb565" : "bgrx8888",
      (format_ == Format::kRGB565 && dither_) ? " +bayer-dither" : "",
      fb_stride_, fb_size_);
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

  // Format dispatch. BGRX8888 → 4 B/px, FlutterToBGRX8888 (memcpy +
  // alpha-force on LE). RGB565 → 2 B/px, FlutterToRGB565 (NEON
  // vld4 + widening-shift + sri on ARM, scalar elsewhere); dither_
  // adds a Bayer 4×4 offset before quantization to hide gradient
  // banding at the cost of bit-exact goldens.
  if (format_ == Format::kRGB565) {
    for (size_t y = 0; y < copy_height; ++y) {
      const uint8_t* src_row = src + y * row_bytes;
      auto* dst_row = reinterpret_cast<uint16_t*>(fb_map_ + y * fb_stride_);
      if (dither_) {
        ivi::swizzle::FlutterToRGB565_BayerDither(dst_row, src_row,
                                                  copy_width_px, y);
      } else {
        ivi::swizzle::FlutterToRGB565(dst_row, src_row, copy_width_px);
      }
      if (copy_width_px < fb_width_) {
        std::memset(dst_row + copy_width_px, 0,
                    (fb_width_ - copy_width_px) * 2);
      }
    }
    for (size_t y = copy_height; y < fb_height_; ++y) {
      std::memset(fb_map_ + y * fb_stride_, 0, fb_width_ * 2);
    }
  } else {
    for (size_t y = 0; y < copy_height; ++y) {
      const uint8_t* src_row = src + y * row_bytes;
      uint8_t* dst_row = fb_map_ + y * fb_stride_;
      ivi::swizzle::FlutterToBGRX8888(dst_row, src_row, copy_width_px);
      if (copy_width_px < fb_width_) {
        std::memset(dst_row + copy_width_px * 4, 0,
                    (fb_width_ - copy_width_px) * 4);
      }
    }
    for (size_t y = copy_height; y < fb_height_; ++y) {
      std::memset(fb_map_ + y * fb_stride_, 0, fb_width_ * 4);
    }
  }
  return true;
}
