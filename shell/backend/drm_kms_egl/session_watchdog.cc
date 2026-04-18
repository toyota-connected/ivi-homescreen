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

#include "backend/drm_kms_egl/session_watchdog.h"

#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xf86drmMode.h>

#include <cerrno>
#include <csignal>

namespace homescreen::watchdog {
namespace {

// Reset handlers inherited from the parent so a stray signal in the child
// can't run parent-installed handlers that touch parent-owned globals
// (spdlog, tty-restore atexit backstop, etc.).
void ResetInheritedSignalHandlers() noexcept {
  constexpr int kSignals[] = {
      SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGABRT,
      SIGILL, SIGFPE,  SIGBUS, SIGPIPE,
  };
  for (const int s : kSignals) {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(s, &sa, nullptr);
  }
}

// Block on ctrl_fd. Returns true when the parent has died (socket EOF);
// returns false when the parent explicitly disarmed us by sending a byte.
bool WaitForParentDeath(const int ctrl_fd) noexcept {
  for (;;) {
    char b;
    const ssize_t n = ::read(ctrl_fd, &b, 1);
    if (n == 0) {
      return true;
    }
    if (n > 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    // Unknown error — don't restore; we can't be sure of our state.
    return false;
  }
}

struct ForkResult {
  pid_t pid;
  int parent_fd;  // writable end, valid only in parent
  int child_fd;   // readable end, valid only in child
};

ForkResult ForkScaffold() noexcept {
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    return {-1, -1, -1};
  }
  // Parent's end is close-on-exec; child's end must stay open after fork.
  ::fcntl(sv[0], F_SETFD, FD_CLOEXEC);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(sv[0]);
    ::close(sv[1]);
    return {-1, -1, -1};
  }
  if (pid == 0) {
    ::close(sv[0]);
    return {0, -1, sv[1]};
  }
  ::close(sv[1]);
  return {pid, sv[0], -1};
}

}  // namespace

Handle SpawnTtyRestore(const int tty_fd, const int saved_kb_mode) noexcept {
  auto [pid, parent_fd, child_fd] = ForkScaffold();
  if (pid < 0) {
    return {};
  }
  if (pid == 0) {
    ResetInheritedSignalHandlers();
    if (WaitForParentDeath(child_fd)) {
      ::ioctl(tty_fd, KDSKBMODE, saved_kb_mode);
    }
    _exit(0);
  }
  return {pid, parent_fd};
}

Handle SpawnDrmRestore(const int drm_fd,
                       const uint32_t crtc_id,
                       const uint32_t buffer_id,
                       const uint32_t x,
                       const uint32_t y,
                       const uint32_t connector_id,
                       const drmModeModeInfo& mode) noexcept {
  // Copy the mode struct so the child has its own post-fork COW page.
  drmModeModeInfo mode_copy = mode;
  auto [pid, parent_fd, child_fd] = ForkScaffold();
  if (pid < 0) {
    return {};
  }
  if (pid == 0) {
    ResetInheritedSignalHandlers();
    if (WaitForParentDeath(child_fd)) {
      uint32_t conn = connector_id;
      drmModeSetCrtc(drm_fd, crtc_id, buffer_id, x, y, &conn, 1, &mode_copy);
    }
    _exit(0);
  }
  return {pid, parent_fd};
}

void Disarm(Handle& handle) noexcept {
  if (handle.control_fd >= 0) {
    // Best-effort disarms byte. MSG_NOSIGNAL prevents a broken-pipe race
    // from killing the parent if the child already exited.
    constexpr char byte = 'Q';
    for (;;) {
      const ssize_t w = ::send(handle.control_fd, &byte, 1, MSG_NOSIGNAL);
      if (w >= 0 || errno != EINTR) {
        break;
      }
    }
    ::close(handle.control_fd);
    handle.control_fd = -1;
  }
  if (handle.pid > 0) {
    int status;
    for (;;) {
      if (const pid_t r = ::waitpid(handle.pid, &status, 0);
          r >= 0 || errno != EINTR) {
        break;
      }
    }
    handle.pid = -1;
  }
}

}  // namespace homescreen::watchdog