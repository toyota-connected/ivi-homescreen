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

#include <sys/types.h>
#include <xf86drmMode.h>

#include <cstdint>

namespace homescreen::watchdog {

// SIGKILL cannot be caught, so in-process atexit/sigaction backstops can't
// restore state if something (or someone) sends signal 9 to the parent.
// The reverse watchdog gets around that: we fork a tiny child that blocks
// on a socketpair. When the parent dies for any reason, the socket closes
// (EOF) and the child does the restore ioctls using fds it inherited.
//
// The child executes only syscalls after fork — no allocator, no logging,
// no pthread state — so it survives the usual post-fork hazards in a
// formerly multithreaded process.
struct Handle {
  pid_t pid = -1;
  int control_fd = -1;  // parent's end of the socketpair
};

// Fork a watchdog that restores the VT keyboard mode to `saved_kb_mode` via
// KDSKBMODE on `tty_fd` if the parent dies. Returns a default-constructed
// Handle (pid < 0) on fork failure.
Handle SpawnTtyRestore(int tty_fd, int saved_kb_mode) noexcept;

// Fork a watchdog that restores the given DRM CRTC with drmModeSetCrtc if
// the parent dies. The child inherits `drm_fd`; because dup-style fd
// inheritance shares the open file description, DRM master status is held
// jointly, so the ioctl still succeeds after the parent's fds close.
Handle SpawnDrmRestore(int drm_fd,
                       uint32_t crtc_id,
                       uint32_t buffer_id,
                       uint32_t x,
                       uint32_t y,
                       uint32_t connector_id,
                       const drmModeModeInfo& mode) noexcept;

// Tell the watchdog to exit without running the restore path — the parent
// has performed cleanup itself — then reap the child. Idempotent.
void Disarm(Handle& handle) noexcept;

}  // namespace homescreen::watchdog