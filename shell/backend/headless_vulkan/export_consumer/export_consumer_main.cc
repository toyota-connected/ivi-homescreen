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

// Standalone same-device test consumer for the ihs-vk-export path. It connects
// to the ihs_carla_bridge's Unix SEQPACKET socket, imports the headless-vulkan
// backend's exported image pool, and for each presented frame waits
// render_done, copies the image to a host buffer and checksums it (proving
// zero-copy content correctness), signals consumer_done and sends a
// FrameRelease -- which drives the backend's consumer-paced vsync.
//
// Env:
//   IHS_VK_CONSUMER_SOCKET  socket path (else $XDG_RUNTIME_DIR/<leaf>, else
//                           /tmp/<leaf>)
//   IHS_VK_CONSUMER_FRAMES  stop after this many consumed frames (default 120)
//
// Exit code: 0 on a clean run (frame budget reached, or a Bye/SIGINT), non-zero
// on any protocol, import, or Vulkan failure.

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vk_consumer.h"
#include "wire_client.h"
#include "wire_protocol.h"

namespace {

volatile sig_atomic_t g_stop = 0;

void OnSigint(int /*sig*/) {
  g_stop = 1;
}

void LogErr(const char* msg) {
  std::fprintf(stderr, "[ihs-vk-consumer] ERROR: %s\n", msg);
}

std::string ResolveSocketPath() {
  if (const char* env = std::getenv("IHS_VK_CONSUMER_SOCKET");
      env != nullptr && env[0] != '\0') {
    return env;
  }
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR");
      xdg != nullptr && xdg[0] != '\0') {
    return std::string(xdg) + "/" + ihs_vke::kDefaultSocketLeaf;
  }
  return std::string("/tmp/") + ihs_vke::kDefaultSocketLeaf;
}

uint32_t ResolveFrameBudget() {
  if (const char* env = std::getenv("IHS_VK_CONSUMER_FRAMES");
      env != nullptr && env[0] != '\0') {
    const long n = std::strtol(env, nullptr, 10);
    if (n > 0) {
      return static_cast<uint32_t>(n);
    }
  }
  return 120;
}

void ToHex(const uint8_t* uuid, char out[33]) {
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    out[2 * i] = kHex[(uuid[i] >> 4) & 0xf];
    out[2 * i + 1] = kHex[uuid[i] & 0xf];
  }
  out[32] = '\0';
}

// Send Hello: the fixed struct followed by the consumer name in one payload.
bool SendHello(int sock, const uint8_t* device_uuid) {
  static constexpr char kName[] = "ihs-vk-export-consumer";
  const uint32_t name_len = static_cast<uint32_t>(std::strlen(kName));

  std::vector<uint8_t> buf(sizeof(ihs_vke::Hello) + name_len);
  ihs_vke::Hello hello{};
  hello.protocol_version = ihs_vke::kProtocolVersion;
  hello.consumer_name_len = name_len;
  std::memcpy(hello.device_uuid, device_uuid, 16);
  std::memcpy(buf.data(), &hello, sizeof(hello));
  std::memcpy(buf.data() + sizeof(hello), kName, name_len);

  return ihs_vke_consumer::SendFramed(sock, ihs_vke::MsgType::kHello,
                                      buf.data(), buf.size(), nullptr, 0);
}

}  // namespace

int main() {
  // Best-effort clean shutdown on Ctrl-C: the poll loop observes g_stop.
  struct sigaction sa{};
  sa.sa_handler = OnSigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART, so poll() returns EINTR promptly
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  const std::string socket_path = ResolveSocketPath();
  const uint32_t frame_budget = ResolveFrameBudget();

  ihs_vke_consumer::VkConsumer consumer;
  if (!consumer.InitVulkan()) {
    LogErr("Vulkan initialization failed");
    return 1;
  }
  char uuid_hex[33];
  ToHex(consumer.device_uuid(), uuid_hex);
  std::fprintf(stderr, "[ihs-vk-consumer] deviceUUID=%s socket=%s frames=%u\n",
               uuid_hex, socket_path.c_str(), frame_budget);

  const int sock = ihs_vke_consumer::ConnectSeqpacket(socket_path.c_str());
  if (sock < 0) {
    std::fprintf(stderr, "[ihs-vk-consumer] ERROR: connect(%s) failed: %s\n",
                 socket_path.c_str(), std::strerror(errno));
    return 1;
  }

  if (!SendHello(sock, consumer.device_uuid())) {
    LogErr("failed to send Hello");
    ::close(sock);
    return 1;
  }

  int exit_code = 0;
  bool table_loaded = false;
  bool caps_ok = false;
  uint32_t consumed = 0;
  bool running = true;

  while (running && !g_stop && consumed < frame_budget) {
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, /*timeout_ms=*/200);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;  // signal -> re-check g_stop
      }
      LogErr("poll failed");
      exit_code = 1;
      break;
    }
    if (pr == 0) {
      continue;  // timeout -> re-check loop conditions
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      std::fprintf(stderr, "[ihs-vk-consumer] socket hung up\n");
      break;
    }

    ihs_vke_consumer::RecvMsg m = ihs_vke_consumer::RecvFramed(sock);
    if (m.closed) {
      std::fprintf(stderr, "[ihs-vk-consumer] peer closed the connection\n");
      break;
    }
    if (!m.ok) {
      LogErr(m.truncated ? "oversized/truncated datagram (protocol error)"
                         : "recv failed");
      exit_code = 1;
      break;
    }

    switch (m.type) {
      case ihs_vke::MsgType::kCaps: {
        if (m.payload.size() < sizeof(ihs_vke::Caps)) {
          LogErr("Caps payload too small");
          exit_code = 1;
          running = false;
          break;
        }
        ihs_vke::Caps caps{};
        std::memcpy(&caps, m.payload.data(), sizeof(caps));
        if (caps.protocol_version != ihs_vke::kProtocolVersion) {
          std::fprintf(stderr,
                       "[ihs-vk-consumer] ERROR: caps protocol_version %u != "
                       "%u\n",
                       caps.protocol_version, ihs_vke::kProtocolVersion);
          exit_code = 1;
          running = false;
          break;
        }
        const bool has_usable =
            (caps.handle_types &
             (ihs_vke::kHandleDmaBuf | ihs_vke::kHandleOpaqueFd)) != 0;
        if (!has_usable) {
          LogErr("caps offer no importable handle type");
          exit_code = 1;
          running = false;
          break;
        }
        char caps_uuid[33];
        ToHex(caps.device_uuid, caps_uuid);
        std::fprintf(stderr,
                     "[ihs-vk-consumer] caps: format=%u fourcc=0x%08x "
                     "extent=%ux%u slots=%u handle_types=0x%x flags=0x%x "
                     "deviceUUID=%s\n",
                     caps.vk_format, caps.drm_fourcc, caps.width, caps.height,
                     caps.slot_count, caps.handle_types, caps.flags, caps_uuid);
        if (std::memcmp(caps.device_uuid, consumer.device_uuid(), 16) != 0) {
          std::fprintf(stderr,
                       "[ihs-vk-consumer] WARNING: caps deviceUUID does not "
                       "match this device; same-device import may fail\n");
        }
        caps_ok = true;
        break;
      }

      case ihs_vke::MsgType::kImageTable: {
        if (!caps_ok) {
          LogErr("ImageTable arrived before a valid Caps");
          exit_code = 1;
          running = false;
          break;
        }
        if (m.payload.size() < sizeof(ihs_vke::ImageTableHeader)) {
          LogErr("ImageTable header too small");
          exit_code = 1;
          running = false;
          break;
        }
        ihs_vke::ImageTableHeader hdr{};
        std::memcpy(&hdr, m.payload.data(), sizeof(hdr));
        const size_t expected =
            sizeof(hdr) +
            static_cast<size_t>(hdr.slot_count) * sizeof(ihs_vke::ImageDesc);
        if (hdr.slot_count == 0 || hdr.slot_count > ihs_vke::kMaxSlots ||
            m.payload.size() < expected ||
            m.fds.size() != ihs_vke::ImageTableFdCount(hdr.slot_count)) {
          LogErr("ImageTable shape mismatch (payload/fd count)");
          exit_code = 1;
          running = false;
          break;
        }
        std::vector<ihs_vke::ImageDesc> descs(hdr.slot_count);
        std::memcpy(
            descs.data(), m.payload.data() + sizeof(hdr),
            static_cast<size_t>(hdr.slot_count) * sizeof(ihs_vke::ImageDesc));

        // Take ownership of the fds out of the RecvMsg so CloseFds cannot
        // double-close them; ImportImageTable now owns every fd (imports or
        // closes each).
        std::vector<int> fds = std::move(m.fds);
        m.fds.clear();

        if (!consumer.ImportImageTable(hdr.generation, descs[0].width,
                                       descs[0].height, descs[0].handle_type,
                                       descs, fds)) {
          LogErr("image table import failed");
          exit_code = 1;
          running = false;
          break;
        }
        table_loaded = true;
        break;
      }

      case ihs_vke::MsgType::kFramePresent: {
        if (m.payload.size() < sizeof(ihs_vke::FramePresent)) {
          LogErr("FramePresent payload too small");
          exit_code = 1;
          running = false;
          break;
        }
        ihs_vke::FramePresent fp{};
        std::memcpy(&fp, m.payload.data(), sizeof(fp));
        if (!table_loaded || fp.generation != consumer.generation()) {
          // Stale generation (e.g. a present that predates our table): skip it
          // rather than wait a semaphore for a slot we have not imported.
          std::fprintf(stderr,
                       "[ihs-vk-consumer] skipping present seq=%u slot=%u "
                       "(generation %u != %u)\n",
                       fp.frame_seq, fp.slot, fp.generation,
                       consumer.generation());
          break;
        }

        ihs_vke_consumer::FrameStats stats{};
        if (!consumer.ConsumePresent(fp.slot, fp.frame_seq, &stats)) {
          LogErr("ConsumePresent failed");
          exit_code = 1;
          running = false;
          break;
        }
        std::fprintf(stderr,
                     "[ihs-vk-consumer] frame seq=%u slot=%u crc=0x%016lx "
                     "px[0]=0x%08x px[center]=0x%08x\n",
                     stats.frame_seq, stats.slot,
                     static_cast<unsigned long>(stats.crc), stats.px_first,
                     stats.px_center);

        // The CPU-side pacing edge the backend's vsync consumes.
        ihs_vke::FrameRelease rel{fp.slot, consumer.generation()};
        if (!ihs_vke_consumer::SendPod(sock, ihs_vke::MsgType::kFrameRelease,
                                       rel)) {
          LogErr("failed to send FrameRelease");
          exit_code = 1;
          running = false;
          break;
        }
        ++consumed;
        break;
      }

      case ihs_vke::MsgType::kBye: {
        uint32_t reason = 0;
        if (m.payload.size() >= sizeof(ihs_vke::Bye)) {
          ihs_vke::Bye bye{};
          std::memcpy(&bye, m.payload.data(), sizeof(bye));
          reason = bye.reason;
        }
        std::fprintf(stderr, "[ihs-vk-consumer] received Bye (reason=%u)\n",
                     reason);
        running = false;
        break;
      }

      default:
        // kInputPointer / kInputKey / kResize are consumer->bridge or reserved;
        // ignore anything unexpected from the bridge.
        break;
    }

    ihs_vke_consumer::CloseFds(m);  // no-op when ownership was moved out
  }

  // Best-effort farewell so the bridge sees an orderly close.
  ihs_vke::Bye bye{static_cast<uint32_t>(ihs_vke::ByeReason::kNormal)};
  ihs_vke_consumer::SendPod(sock, ihs_vke::MsgType::kBye, bye);

  consumer.Teardown();
  ::close(sock);

  std::fprintf(stderr,
               "[ihs-vk-consumer] done: consumed %u frame(s), exit=%d\n",
               consumed, exit_code);
  return exit_code;
}
