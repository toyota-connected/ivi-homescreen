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

#include "wire_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace ihs_vke_consumer {

namespace {
// The most fds any one message carries (a full-pool image table): three per
// slot -- memory, render_done, consumer_done.
constexpr size_t kMaxFds = 3 * ihs_vke::kMaxSlots;

// Largest payload the protocol defines (an image table for a full pool), plus
// slack for a Hello name. Sizes the receive buffer; a larger datagram is a
// protocol error (reported as truncated).
constexpr size_t kMaxPayloadBytes =
    sizeof(ihs_vke::ImageTableHeader) +
    ihs_vke::kMaxSlots * sizeof(ihs_vke::ImageDesc) + 256;

ssize_t RetryingSendmsg(int sock, const msghdr* msg) {
  ssize_t n;
  do {
    n = ::sendmsg(sock, msg, MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  return n;
}

ssize_t RetryingRecvmsg(int sock, msghdr* msg) {
  ssize_t n;
  do {
    n = ::recvmsg(sock, msg, MSG_CMSG_CLOEXEC);
  } while (n < 0 && errno == EINTR);
  return n;
}
}  // namespace

int ConnectSeqpacket(const char* path) {
  if (path == nullptr) {
    errno = EINVAL;
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (std::strlen(path) >= sizeof(addr.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  const int sock = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    return -1;
  }
  if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int saved = errno;
    ::close(sock);
    errno = saved;
    return -1;
  }
  return sock;
}

bool SendFramed(int sock,
                ihs_vke::MsgType type,
                const void* payload,
                size_t payload_len,
                const int* fds,
                size_t nfds) {
  if (nfds > kMaxFds || (payload_len != 0 && payload == nullptr)) {
    return false;
  }
  ihs_vke::MsgHeader hdr{static_cast<uint32_t>(type),
                         static_cast<uint32_t>(payload_len)};

  iovec iov[2];
  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = const_cast<void*>(payload);
  iov[1].iov_len = payload_len;

  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = payload_len != 0 ? 2 : 1;

  // Ancillary SCM_RIGHTS block for the fds (only when there are any).
  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
  if (nfds > 0) {
    std::memset(cbuf, 0, sizeof(cbuf));
    msg.msg_control = cbuf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
    std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
    msg.msg_controllen = cm->cmsg_len;  // exact length the kernel expects
  }

  const ssize_t sent = RetryingSendmsg(sock, &msg);
  // SEQPACKET delivers the whole datagram or nothing; a short count is an
  // error.
  return sent == static_cast<ssize_t>(sizeof(hdr) + payload_len);
}

RecvMsg RecvFramed(int sock) {
  RecvMsg out;

  ihs_vke::MsgHeader hdr{};
  std::vector<uint8_t> buf(kMaxPayloadBytes);
  iovec iov[2];
  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = buf.data();
  iov[1].iov_len = buf.size();

  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);

  const ssize_t n = RetryingRecvmsg(sock, &msg);
  if (n == 0) {
    out.closed = true;  // orderly peer shutdown
    return out;
  }
  if (n < 0) {
    return out;  // ok=false; errno holds the reason
  }

  // Collect any fds first so every error path can close them.
  for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
       cm = CMSG_NXTHDR(&msg, cm)) {
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
      const size_t bytes = cm->cmsg_len - CMSG_LEN(0);
      const size_t count = bytes / sizeof(int);
      const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cm));
      for (size_t i = 0; i < count; ++i) {
        out.fds.push_back(fds[i]);
      }
    }
  }

  // A truncated payload or dropped fds means the sender exceeded our limits --
  // treat as a protocol error and discard (closing any fds we did receive).
  if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
      n < static_cast<ssize_t>(sizeof(hdr)) ||
      hdr.len != static_cast<uint32_t>(n) - sizeof(hdr)) {
    out.truncated = true;
    CloseFds(out);
    return out;
  }

  out.type = static_cast<ihs_vke::MsgType>(hdr.type);
  out.payload.assign(buf.begin(), buf.begin() + hdr.len);
  out.ok = true;
  return out;
}

void CloseFds(RecvMsg& m) {
  for (int fd : m.fds) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  m.fds.clear();
}

}  // namespace ihs_vke_consumer
