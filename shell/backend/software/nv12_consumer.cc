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

// INv12Consumer implementations. This TU is the only one in the software
// backend that links the V4L2 hardware encoder, so the sink itself stays free
// of that dependency. The WebRTC-send consumer (which links libwebrtc) will be
// added here behind the same seam.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backend/software/encoder_sink.h"
#include "config/common.h"  // BUILD_SOFTWARE_SINK_ENCODER_WEBRTC
#include "logging/logging.h"

// The V4L2 M2M encoder from the v4l2-webrtc-codec repo (webrtc-free). Its path
// is put on the include path by the BUILD_SOFTWARE_SINK_ENCODER CMake branch.
#include "src/v4l2_m2m_encoder.h"

#if BUILD_SOFTWARE_SINK_ENCODER_WEBRTC
// Defined in webrtc_consumer.cc (the only TU that links libwebrtc).
std::unique_ptr<INv12Consumer> MakeWebRtcConsumer(std::string_view spec);
#endif

namespace {

const char* EncodeDevice() {
  const char* dev = std::getenv("IVI_ENC_DEVICE");
  return (dev != nullptr && dev[0] != '\0') ? dev : "/dev/video11";
}

uint32_t EnvU32(const char* name, uint32_t fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return fallback;
  }
  // strtoul silently wraps a leading '-', so reject a sign up front.
  if (v[0] == '-' || v[0] == '+') {
    return fallback;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(v, &end, 10);
  if (errno != 0 || end == v || *end != '\0' || parsed == 0 ||
      parsed > UINT32_MAX) {
    return fallback;  // out of range / trailing junk / not a positive integer
  }
  return static_cast<uint32_t>(parsed);
}

// Drives the V4L2 hardware encoder and appends each coded H.264 access unit to
// an Annex-B file. Validation consumer: proves Flutter -> BGRA -> NV12 -> HW
// encode inside the shell, no networking.
class FileEncoderConsumer final : public INv12Consumer {
 public:
  explicit FileEncoderConsumer(std::string path) : path_(std::move(path)) {}

  ~FileEncoderConsumer() override {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
    if (frames_ != 0) {
      ihs::log::info(
          "[EncoderSink/file] wrote {} H.264 frames ({} bytes) to {}", frames_,
          bytes_, path_);
    }
  }

  bool Configure(uint32_t width, uint32_t height, uint32_t stride) override {
    stride_ = stride;
    height_ = height;
    const uint32_t bitrate = EnvU32("IVI_ENC_BITRATE", 4'000'000);
    const uint32_t fps = EnvU32("IVI_ENC_FPS", 30);
    // Release any prior encoder before reopening the device: a reconfigure
    // (resize) must not hold two encoders on the same V4L2 node at once.
    encoder_.reset();
    encoder_ = v4l2wc::V4l2M2mEncoder::Create(EncodeDevice(), width, height,
                                              bitrate, fps, fps * 2);
    if (!encoder_) {
      ihs::log::error("[EncoderSink/file] encoder open {} failed ({}x{})",
                      EncodeDevice(), width, height);
      return false;
    }
    // Open the capture once. A reconfigure keeps writing to the same file, so
    // the new stream -- which starts with a fresh IDR (the sink forces a
    // keyframe on the first frame after each configure) -- appends at the
    // resolution change rather than truncating and discarding what was already
    // captured, and the FILE* is never leaked by reopening.
    if (file_ == nullptr) {
      file_ = std::fopen(path_.c_str(), "wb");
      if (file_ == nullptr) {
        ihs::log::error("[EncoderSink/file] cannot open {} for write: {}",
                        path_, std::strerror(errno));
        encoder_.reset();  // don't leave the V4L2 device open half-configured
        return false;
      }
    }
    ihs::log::info("[EncoderSink/file] encoding {}x{} -> {} via {}", width,
                   height, path_, EncodeDevice());
    return true;
  }

  // Synchronous: EncodeDmabuf completes before we return, so we never hold the
  // frame -- return false and the sink reclaims the ring slot itself (we take
  // neither the buffer nor the release).
  bool OnFrame(int dmabuf_fd,
               const uint8_t* /*nv12*/,
               uint64_t timestamp_us,
               bool force_keyframe,
               void (* /*release*/)(void*),
               void* /*release_ctx*/) override {
    // Once a write has failed the file is already truncated, so stop encoding:
    // further EncodeDmabuf()/fwrite() work only burns CPU/IO on a corrupt file.
    if (!encoder_ || file_ == nullptr || write_failed_) {
      return false;
    }
    int fds[2] = {dmabuf_fd, dmabuf_fd};
    uint32_t offsets[2] = {0, stride_ * height_};
    uint32_t strides[2] = {stride_, stride_};
    au_.clear();
    bool keyframe = false;
    if (!encoder_->EncodeDmabuf(fds, offsets, strides, 2, timestamp_us,
                                force_keyframe, &au_, &keyframe)) {
      ihs::log::warn(
          "[EncoderSink/file] encode failed ({} frames written so far)",
          frames_);
      return false;
    }
    if (!au_.empty()) {
      const size_t wrote = std::fwrite(au_.data(), 1, au_.size(), file_);
      if (wrote != au_.size()) {
        // Short write (disk full / IO error): the file is now truncated, so
        // warn once and stop counting rather than silently produce corruption.
        if (!write_failed_) {
          ihs::log::error(
              "[EncoderSink/file] short write to {} ({} of {} bytes); output "
              "is "
              "truncated",
              path_, wrote, au_.size());
          write_failed_ = true;
        }
        return false;
      }
      bytes_ += au_.size();
      ++frames_;
    }
    return false;
  }

 private:
  std::string path_;
  std::unique_ptr<v4l2wc::V4l2M2mEncoder> encoder_;
  std::FILE* file_{nullptr};
  std::vector<uint8_t> au_;
  uint32_t stride_{0};
  uint32_t height_{0};
  uint64_t frames_{0};
  uint64_t bytes_{0};
  bool write_failed_{false};
};

}  // namespace

std::unique_ptr<INv12Consumer> MakeNv12Consumer(std::string_view spec) {
  if (spec.empty()) {
    ihs::log::warn(
        "[EncoderSink] the encoder sink needs a consumer, e.g. "
        "IVI_SW_SINK=encoder:file:<path>");
    return nullptr;
  }
  constexpr std::string_view kFilePrefix = "file:";
  if (spec.rfind(kFilePrefix, 0) == 0) {
    std::string path(spec.substr(kFilePrefix.size()));
    if (path.empty()) {
      ihs::log::warn("[EncoderSink] 'encoder:file:' needs a path");
      return nullptr;
    }
    return std::make_unique<FileEncoderConsumer>(std::move(path));
  }
  constexpr std::string_view kWebrtcPrefix = "webrtc:";
  if (spec.rfind(kWebrtcPrefix, 0) == 0) {
#if BUILD_SOFTWARE_SINK_ENCODER_WEBRTC
    return MakeWebRtcConsumer(spec.substr(kWebrtcPrefix.size()));
#else
    ihs::log::warn(
        "[EncoderSink] webrtc consumer requested but compiled without "
        "BUILD_SOFTWARE_SINK_ENCODER_WEBRTC");
    return nullptr;
#endif
  }
  ihs::log::warn(
      "[EncoderSink] unrecognized consumer spec '{}' (valid: file:<path> | "
      "webrtc:<host>:<port>)",
      std::string(spec));
  return nullptr;
}
