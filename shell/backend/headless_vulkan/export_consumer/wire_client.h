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

// Minimal, self-contained SEQPACKET framing for the ihs-vk-export test
// consumer (the client half of wire_protocol.h). One message is one datagram:
// an 8-byte MsgHeader { type, len } followed by `len` payload bytes, with any
// file descriptors carried as ancillary SCM_RIGHTS data on the same datagram.
// The consumer only needs to send Hello / FrameRelease / Bye and to receive
// Caps / ImageTable / FramePresent / Bye, so this is intentionally smaller than
// the bridge's transport -- it lives entirely inside the consumer.

#ifndef SHELL_BACKEND_HEADLESS_VULKAN_EXPORT_CONSUMER_WIRE_CLIENT_H_
#define SHELL_BACKEND_HEADLESS_VULKAN_EXPORT_CONSUMER_WIRE_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wire_protocol.h"

namespace ihs_vke_consumer {

// Connect an AF_UNIX SOCK_SEQPACKET client socket to `path`. Returns the
// connected fd (CLOEXEC) or -1 on failure (errno set).
int ConnectSeqpacket(const char* path);

// Send one framed message: header{type,len} + payload, with `nfds` descriptors
// as SCM_RIGHTS. Returns true iff the whole datagram was sent. Does not take
// ownership of the fds (the caller keeps and closes its own copies).
bool SendFramed(int sock,
                ihs_vke::MsgType type,
                const void* payload,
                size_t payload_len,
                const int* fds,
                size_t nfds);

// Convenience: send a POD payload with no fds.
template <typename T>
inline bool SendPod(int sock, ihs_vke::MsgType type, const T& payload) {
  return SendFramed(sock, type, &payload, sizeof(T), nullptr, 0);
}

// One received datagram. Any fds are dup()-owned by this struct; close each
// when done (CloseFds, or transfer ownership out).
struct RecvMsg {
  ihs_vke::MsgType type{};
  std::vector<uint8_t> payload;
  std::vector<int> fds;
  bool ok = false;         // a well-formed message was received
  bool closed = false;     // the peer closed the connection (ok == false)
  bool truncated = false;  // payload or fds did not fit (protocol error)
};

// Receive exactly one datagram. Never blocks past one message. On a clean peer
// close returns { ok=false, closed=true }. On a short/oversized datagram
// returns { ok=false, truncated=true } and closes any fds it did receive. Uses
// MSG_CMSG_CLOEXEC so received fds are not leaked across an exec.
RecvMsg RecvFramed(int sock);

// Close every fd still held by a RecvMsg (helper for error/drop paths).
void CloseFds(RecvMsg& m);

}  // namespace ihs_vke_consumer

#endif  // SHELL_BACKEND_HEADLESS_VULKAN_EXPORT_CONSUMER_WIRE_CLIENT_H_
