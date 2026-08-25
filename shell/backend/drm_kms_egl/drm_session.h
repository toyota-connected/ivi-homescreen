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

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "input/wake_event_fd.h"

#include <drm-cxx/display/hotplug_monitor.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/session/seat.hpp>

namespace homescreen {

// Process-wide libseat holder. When constructed successfully, DrmBackend
// opens its DRM device via TakeDevice (revocable fd handed out by
// logind/seatd/builtin) and DrmSeat routes libinput's privileged
// /dev/input/event* opens through InputOpener(). A background thread
// services Seat::dispatch() so pause/resume callbacks fire without
// consumers having to integrate poll_fd() into their own event loops.
//
// Pause/resume is real: DrmBackend installs lifecycle handlers via
// SetLifecycleHandlers(), and the DRM device is taken with
// preserve_fd_across_resume so logind/seatd's capability-revoke leaves
// the fd integer valid across the round-trip. The consumer just halts
// commits while paused and re-attaches MODE_ID/ACTIVE on the next
// commit after resume. Until handlers are installed (early init, or
// when running without DrmBackend), the fallback is to raise SIGTERM
// so the process exits cleanly rather than wedging.
//
// Open() returns nullptr when no seat backend is available (no logind,
// no seatd, no permissions for builtin, or drm-cxx built without libseat
// support). Callers then fall back to opening DRM / input devices
// directly, with the pre-libseat guards (foreground-VT check,
// drmSetMaster) providing roughly the same activation guarantees the
// seat provider would. The SIGKILL-recovery reverse watchdog that used
// to back the fallback path has been removed; on a SIGKILL'd bare-TTY
// shell the VT may be stranded in K_OFF and the CRTC pointing at a
// freed BO until the next compositor reprograms — recover manually
// with `sudo kbd_mode -a` and a `chvt` round-trip.
class DrmSession {
 public:
  static std::unique_ptr<DrmSession> Open();

  ~DrmSession();

  DrmSession(const DrmSession&) = delete;
  DrmSession& operator=(const DrmSession&) = delete;

  // Returns a revocable fd for `path`, or -1 on failure. Ownership stays
  // with libseat; do not ::close() the returned fd. Wrap in a drm::Device
  // via drm::Device::from_fd(). The fd is preserved across pause/resume
  // (logind/seatd revoke the master capability without invalidating the
  // fd integer), so callers can keep their per-fd state alive and just
  // re-modeset after OnResume.
  [[nodiscard]] int TakeDevice(const std::string& path);

  // Opener to hand to drm::input::Seat::open so libinput's
  // open_restricted/close_restricted route through libseat.
  [[nodiscard]] drm::input::InputDeviceOpener InputOpener();

  // Request a session switch (VT number; F1=1, F2=2, …). Routed through
  // libseat so it works in K_OFF where the kernel chord handler is bypassed.
  // Returns 0 on success, an errno on failure (notably ENOTSUP when this
  // build has no libseat backend).
  [[nodiscard]] int SwitchSession(int session_num);

  // Lifecycle handlers. Subscribers are typically DrmBackend (gates the
  // compositor + asks the engine to ScheduleFrame on resume) and
  // DrmSeat (libinput_suspend / libinput_resume so post-resume input
  // devices use fresh fds instead of pre-pause stale ones). Until at
  // least one pause handler is installed, the default behavior is to
  // raise SIGTERM on pause so an uninitialized process exits cleanly
  // rather than wedging. All handlers fire on the libseat dispatch
  // thread in registration order; they must do only fast, lock-friendly
  // work (set atomics, post to another thread). The fd passed to
  // resume handlers is the same fd as the initial TakeDevice
  // (preserve-fd contract).
  using PauseFn = std::function<void()>;
  using ResumeFn = std::function<void(int fd)>;
  void AddPauseHandler(PauseFn on_pause);
  void AddResumeHandler(ResumeFn on_resume);

  // Install or replace a handler invoked on every DRM hotplug uevent
  // (connector plug/unplug). Fires on the dispatch thread, so the
  // handler must be thread-safe with respect to anything it touches.
  // No-op if the underlying HotplugMonitor failed to open or was
  // disabled via IVI_DRM_HOTPLUG=0.
  using HotplugHandler = std::function<void(const drm::display::HotplugEvent&)>;
  void set_hotplug_handler(HotplugHandler handler);

 private:
  explicit DrmSession(drm::session::Seat seat);
  void DispatchLoop();

  drm::session::Seat seat_;
  std::optional<drm::display::HotplugMonitor> hotplug_;
  std::mutex hotplug_handler_mu_;
  HotplugHandler hotplug_handler_;
  // Lifecycle handler lists + their mutex. Guards installation racing
  // with the dispatch thread reading them inside the libseat trampoline.
  // The dispatch loop snapshots the lists under the mutex before
  // invoking, so handlers run lock-free (they may themselves call back
  // into DrmSession without re-entering this mutex).
  std::mutex lifecycle_mu_;
  std::vector<PauseFn> pause_handlers_;
  std::vector<ResumeFn> resume_handlers_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  // Breaks the dispatch loop out of its indefinite poll at teardown. Without
  // it the loop needs a timeout purely to re-check stop_, which costs a wakeup
  // several times a second on a system that is doing nothing.
  input::WakeEventFd waker_;
};

}  // namespace homescreen
