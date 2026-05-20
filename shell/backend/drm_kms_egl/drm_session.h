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
// Pause/resume is stubbed: on VT switch-out we raise SIGTERM and let the
// existing shutdown path run (CRTC restore, EGL/GBM teardown, VT keyboard
// restore). A follow-up pass will implement real revocation handling —
// tearing down per-fd state and rebuilding on the new fd handed back by
// the resume callback.
//
// Open() returns nullptr when no seat backend is available (no logind,
// no seatd, no permissions for builtin, or drm-cxx built without libseat
// support). Callers then fall back to opening DRM / input devices
// directly, with the pre-libseat guards (foreground-VT check,
// drmSetMaster, reverse watchdogs) providing roughly the same guarantees
// the seat provider would.
class DrmSession {
 public:
  static std::unique_ptr<DrmSession> Open();

  ~DrmSession();

  DrmSession(const DrmSession&) = delete;
  DrmSession& operator=(const DrmSession&) = delete;

  // Returns a revocable fd for `path`, or -1 on failure. Ownership stays
  // with libseat; do not ::close() the returned fd. Wrap in a drm::Device
  // via drm::Device::from_fd().
  [[nodiscard]] int TakeDevice(const std::string& path);

  // Opener to hand to drm::input::Seat::open so libinput's
  // open_restricted/close_restricted route through libseat.
  [[nodiscard]] drm::input::InputDeviceOpener InputOpener();

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
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace homescreen
