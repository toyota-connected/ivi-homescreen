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

// The WebRTC send consumer: hands each NV12 dma-buf to libwebrtc as a native
// frame (lw_video_source_push_dmabuf), so the factory's V4L2 hardware encoder
// (selected by LW_V4L2_ENCODE) turns it into H.264 + RTP to a remote peer. It
// is the inverse of ihs_webrtc_view's receive session, and the only software-
// backend TU that links libwebrtc -- gated behind BUILD_SOFTWARE_SINK_ENCODER
// _WEBRTC. The offerer logic mirrors the validated send_session harness.
//
// Signaling is the same length-framed TCP protocol as the receive side
// (OFFER/ANSWER/CAND); we connect out to the peer's TCP server.

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "backend/software/encoder_sink.h"
#include "logging/logging.h"

// The fork's flat C ABI (headers via LIBWEBRTC_DIR/include, .so linked by the
// BUILD_SOFTWARE_SINK_ENCODER_WEBRTC CMake branch).
#include "c/lw_c_api.h"
#include "c/lw_video_sink.h"

namespace {

constexpr uint32_t kDrmFormatNv12 = 0x3231564EU;  // 'N','V','1','2'

int ParsePort(std::string_view s, int fallback) {
  // Strict parse: the whole string must be a number in 1..65535. from_chars
  // rejects a trailing garbage tail (ptr != end) and overflow (ec set), unlike
  // atoi which parses partially and is UB on overflow.
  int value = 0;
  const char* const end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, value);
  if (ec != std::errc{} || ptr != end || value <= 0 || value > 65535) {
    return fallback;
  }
  return value;
}

// Offerer WebRTC send session driven by pushed NV12 dma-bufs.
class WebRtcSenderConsumer final : public INv12Consumer {
 public:
  WebRtcSenderConsumer(std::string host, int port)
      : host_(std::move(host)), port_(port) {}

  ~WebRtcSenderConsumer() override { Stop(); }

  bool Configure(uint32_t width, uint32_t height, uint32_t stride) override {
    width_ = width;
    height_ = height;
    stride_ = stride;
    if (started_) {
      return true;  // session already up; a resize just re-sizes the frames
    }
    // The hardware encoder is chosen at factory init from this env var.
    setenv("LW_V4L2_ENCODE", "1", 1);
    if (!lw_initialize()) {
      ihs::log::error("[EncoderSink/webrtc] lw_initialize failed");
      return false;
    }
    lw_initialized_ = true;
    factory_ = lw_factory_create();
    if (factory_ == nullptr || lw_factory_initialize(factory_) == 0) {
      ihs::log::error("[EncoderSink/webrtc] factory init failed");
      Stop();
      return false;
    }
    pc_ = lw_pc_create(factory_);
    if (pc_ == nullptr) {
      ihs::log::error("[EncoderSink/webrtc] pc_create failed");
      Stop();
      return false;
    }

    // obs_ is a member, not a local: even though the C ABI copies the struct,
    // keeping it alive for the session removes any dependency on that copy.
    std::memset(&obs_, 0, sizeof obs_);
    obs_.size = sizeof obs_;
    obs_.on_ice_candidate = &OnIceCandidate;
    obs_.on_ice_connection_state = &OnIceState;
    lw_pc_set_observer(pc_, &obs_, this);

    source_ = lw_factory_create_video_source(factory_, "ivi-homescreen");
    track_ = lw_factory_create_video_track(factory_, source_, "v0");
    const char* streams[] = {"ivi"};
    lw_pc_add_track(pc_, track_, streams, 1);

    if (!ConnectSignaling()) {
      Stop();
      return false;
    }
    running_ = true;
    reader_ = std::thread(&WebRtcSenderConsumer::ReaderLoop, this);
    lw_pc_create_offer(pc_, &OnOfferCreated, &OnSdpFail, this);
    started_ = true;
    ihs::log::info("[EncoderSink/webrtc] session up -> {}:{} ({}x{})", host_,
                   port_, width_, height_);
    return true;
  }

  // Async: push the dma-buf into libwebrtc, which encodes it on its own thread
  // and fires `release` post-encode -- exactly the seam's take-and-release
  // contract, so we return true and hand the release straight through.
  bool OnFrame(int dmabuf_fd,
               const uint8_t* /*nv12*/,
               uint64_t timestamp_us,
               bool /*force_keyframe*/,
               void (*release)(void*),
               void* release_ctx) override {
    if (source_ == nullptr) {
      return false;  // no session: let the sink reclaim the slot
    }
    LwDmabufDescriptor d;
    std::memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.fourcc = kDrmFormatNv12;
    d.modifier = 0;
    d.width = width_;
    d.height = height_;
    d.num_planes = 2;
    d.planes[0].fd = dmabuf_fd;
    d.planes[0].offset = 0;
    d.planes[0].pitch = stride_;
    d.planes[1].fd = dmabuf_fd;
    d.planes[1].offset = stride_ * height_;
    d.planes[1].pitch = stride_;
    d.acquire_fence_fd = -1;
    d.rtp_timestamp_us = static_cast<int64_t>(timestamp_us);
    // On success the source takes the release obligation (fires post-encode);
    // on rejection it does not, so the sink must reclaim the slot.
    const int rc =
        lw_video_source_push_dmabuf(source_, &d, release, release_ctx);
    if (pushed_++ == 0 || rc != 0) {
      ihs::log::info(
          "[EncoderSink/webrtc] push_dmabuf #{} {}x{} fd={} -> rc={}", pushed_,
          width_, height_, dmabuf_fd, rc);
    }
    return rc == 0;
  }

 private:
  // ---- observer + sdp callbacks (signaling thread) ----
  static WebRtcSenderConsumer* Self(void* user) {
    return static_cast<WebRtcSenderConsumer*>(user);
  }
  static void OnIceCandidate(char* candidate,
                             char* sdp_mid,
                             int mline,
                             void* user) {
    if (candidate != nullptr) {
      char hdr[64];
      const int n = static_cast<int>(std::strlen(candidate));
      std::snprintf(hdr, sizeof hdr, "CAND %d %d\n", mline, n);
      Self(user)->SendFramed(hdr, candidate, n);
    }
    lw_string_free(candidate);
    lw_string_free(sdp_mid);
  }
  static void OnIceState(int state, void* user) {
    Self(user)->ice_state_ = state;
    ihs::log::info("[EncoderSink/webrtc] ice_connection_state={}", state);
  }
  static void OnSdpFail(char* err, void* /*user*/) {
    ihs::log::warn("[EncoderSink/webrtc] sdp error: {}", err ? err : "?");
    lw_string_free(err);
  }
  static void OnOfferCreated(char* sdp, char* type, void* user) {
    auto* self = Self(user);
    if (sdp != nullptr) {
      lw_pc_set_local_description(self->pc_, sdp, "offer", nullptr, &OnSdpFail,
                                  user);
      char hdr[64];
      const int n = static_cast<int>(std::strlen(sdp));
      std::snprintf(hdr, sizeof hdr, "OFFER %d\n", n);
      self->SendFramed(hdr, sdp, n);
      ihs::log::info("[EncoderSink/webrtc] offer sent ({} bytes)", n);
    }
    lw_string_free(sdp);
    lw_string_free(type);
  }

  // ---- signaling socket ----
  bool ConnectSignaling() {
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    std::snprintf(portstr, sizeof portstr, "%d", port_);
    addrinfo* res = nullptr;
    const int gai = getaddrinfo(host_.c_str(), portstr, &hints, &res);
    if (gai != 0) {
      ihs::log::error("[EncoderSink/webrtc] resolve {}:{} failed: {}", host_,
                      port_, gai_strerror(gai));
      return false;
    }
    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) {
      ihs::log::error("[EncoderSink/webrtc] socket() failed: {}",
                      std::strerror(errno));
      freeaddrinfo(res);
      return false;
    }
    if (connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
      ihs::log::error("[EncoderSink/webrtc] connect {}:{} failed: {}", host_,
                      port_, std::strerror(errno));
      close(sock_);  // connect() failed on an open fd; don't leak it
      sock_ = -1;
      freeaddrinfo(res);
      return false;
    }
    freeaddrinfo(res);
    return true;
  }
  // send() may complete partially on a TCP socket; a short write here would
  // desync the length-framed protocol, so loop until all bytes are sent.
  // MSG_NOSIGNAL: a peer disconnect must not raise SIGPIPE and kill the shell.
  static bool WriteAll(int fd, const char* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
      const ssize_t r = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
      if (r < 0 && errno == EINTR) {
        continue;  // interrupted by a signal before sending; retry
      }
      if (r <= 0) {
        return false;
      }
      sent += static_cast<size_t>(r);
    }
    return true;
  }
  void SendFramed(const char* header, const char* payload, int len) {
    // send_mu_ also serializes with Stop() closing sock_, so the fd can't be
    // closed under us and sock_ is read consistently.
    std::lock_guard<std::mutex> lock(send_mu_);
    if (sock_ < 0) {
      return;
    }
    if (!WriteAll(sock_, header, std::strlen(header))) {
      return;
    }
    if (len > 0) {
      WriteAll(sock_, payload, static_cast<size_t>(len));
    }
  }
  // Returns the line length (>=0), kEof when the peer closed the socket, or -1
  // on a timeout/error. Distinguishing EOF lets ReaderLoop stop instead of
  // spinning: after a disconnect read() returns 0 immediately every call.
  static constexpr int kEof = -2;
  static int ReadLine(int fd, char* out, int max) {
    int i = 0;
    while (i < max - 1) {
      char c;
      const ssize_t r = read(fd, &c, 1);
      if (r < 0 && errno == EINTR) {
        continue;  // interrupted by a signal; retry the read
      }
      if (r == 0) {
        return kEof;
      }
      if (r < 0) {
        return -1;
      }
      if (c == '\n') {
        break;
      }
      out[i++] = c;
    }
    out[i] = 0;
    return i;
  }
  static int ReadN(int fd, char* buf, int n) {
    int got = 0;
    while (got < n) {
      const ssize_t r = read(fd, buf + got, n - got);
      if (r < 0 && errno == EINTR) {
        continue;  // interrupted by a signal; retry the read
      }
      if (r <= 0) {
        return -1;
      }
      got += static_cast<int>(r);
    }
    return got;
  }
  void ReaderLoop() {
    char header[128];
    std::unique_ptr<char[]> payload(new char[1 << 16]);
    while (running_) {
      timeval tv = {1, 0};
      setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      const int hlen = ReadLine(sock_, header, sizeof header);
      if (hlen == kEof) {
        break;  // peer closed the signaling channel; stop rather than spin
      }
      if (hlen <= 0) {
        continue;  // timeout or empty line: retry while running_
      }
      char kind[16];
      int a = 0;
      int b = 0;
      if (std::sscanf(header, "%15s", kind) != 1) {
        continue;
      }
      if (std::strcmp(kind, "ANSWER") == 0) {
        std::sscanf(header, "%*s %d", &b);
        if (b > 0 && b < (1 << 16) && ReadN(sock_, payload.get(), b) == b) {
          payload[b] = 0;
          lw_pc_set_remote_description(pc_, payload.get(), "answer", nullptr,
                                       &OnSdpFail, this);
          ihs::log::info("[EncoderSink/webrtc] answer applied ({} bytes)", b);
        }
      } else if (std::strcmp(kind, "CAND") == 0) {
        std::sscanf(header, "%*s %d %d", &a, &b);
        if (b > 0 && b < (1 << 16) && ReadN(sock_, payload.get(), b) == b) {
          payload[b] = 0;
          lw_pc_add_ice_candidate(pc_, "", a, payload.get());
        }
      }
    }
  }

  void Stop() {
    if (track_ != nullptr) {
      LwVideoTrackStats st;
      std::memset(&st, 0, sizeof st);
      st.size = sizeof st;
      if (lw_video_track_get_stats(track_, &st) == 0) {
        ihs::log::info(
            "[EncoderSink/webrtc] pushed={} track: delivered={} native={} "
            "cpu={} dropped={} ice={}",
            pushed_, st.frames_delivered, st.frames_native, st.frames_cpu,
            st.frames_dropped, ice_state_.load());
      }
    }
    running_ = false;
    if (reader_.joinable()) {
      reader_.join();
    }
    // Idempotent teardown: null each handle after releasing it so a Configure()
    // failure path and the destructor can both call Stop() safely, and every
    // created object is released even when Configure() bailed part-way through.
    if (track_ != nullptr) {
      lw_release(track_);
      track_ = nullptr;
    }
    if (source_ != nullptr) {
      lw_release(source_);
      source_ = nullptr;
    }
    if (pc_ != nullptr) {
      lw_pc_remove_observer(pc_);
      lw_pc_close(pc_);
      lw_release(pc_);
      pc_ = nullptr;
    }
    if (factory_ != nullptr) {
      lw_release(factory_);
      factory_ = nullptr;
    }
    // lw_terminate must pair with a successful lw_initialize(), tracked apart
    // from started_ (which is only set once Configure() fully succeeds).
    if (lw_initialized_) {
      lw_terminate();
      lw_initialized_ = false;
    }
    // Close the socket under send_mu_ so it cannot race a SendFramed() call
    // that a signaling-thread observer callback may still be making.
    {
      std::lock_guard<std::mutex> lock(send_mu_);
      if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
      }
    }
  }

  std::string host_;
  int port_;
  int sock_{-1};
  std::mutex send_mu_;
  std::thread reader_;
  std::atomic<bool> running_{false};
  bool lw_initialized_{
      false};  // lw_initialize() succeeded (pairs lw_terminate)
  bool started_{false};
  std::atomic<int> ice_state_{0};

  lw_factory_t* factory_{nullptr};
  lw_pc_t* pc_{nullptr};
  lw_video_source_t* source_{nullptr};
  lw_video_track_t* track_{nullptr};
  LwPcObserver obs_{};  // outlives the lw_pc_set_observer registration on pc_

  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t stride_{0};
  uint64_t pushed_{0};
};

}  // namespace

// Called by MakeNv12Consumer for a "webrtc:<host>:<port>" spec. Kept as a free
// function so the file-consumer TU can dispatch to it only when this TU (and
// libwebrtc) is compiled in.
std::unique_ptr<INv12Consumer> MakeWebRtcConsumer(std::string_view spec) {
  // spec is "<host>:<port>" (or just "<host>", default port 9000).
  std::string s(spec);
  const auto colon = s.rfind(':');
  std::string host = colon == std::string::npos ? s : s.substr(0, colon);
  const int port =
      colon == std::string::npos ? 9000 : ParsePort(s.substr(colon + 1), 9000);
  if (host.empty()) {
    ihs::log::warn(
        "[EncoderSink] webrtc consumer needs a host, e.g. "
        "encoder:webrtc:<host>:<port>");
    return nullptr;
  }
  return std::make_unique<WebRtcSenderConsumer>(std::move(host), port);
}
