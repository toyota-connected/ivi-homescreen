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
#include <cstdint>
#include <thread>

#include <xkbcommon/xkbcommon.h>

#include <shell/platform/embedder/embedder.h>

#include "input/iseat.h"

struct libinput;
struct libinput_event;
struct libinput_event_pointer;
struct libinput_event_keyboard;
struct udev;

namespace homescreen {

// libinput-backed input source for the software backend.
//
// Owns a libinput context built against udev seat "seat0", a worker
// thread that polls libinput's fd, an xkbcommon context for key
// translation, and the running pointer / button state. Translates
// libinput events into FlutterPointerEvent (via SendPointerEvent) and
// keyboard events (via the project-wide KeyCallback).
//
// Devices opened with ::open under the user's own credentials —
// operator must be in the `input` group on systemd distros. Future
// libseat integration could route opens through a seat session.
class SoftwareSeat final : public ISeat {
 public:
  SoftwareSeat(int32_t viewport_width, int32_t viewport_height);
  ~SoftwareSeat() override;

  bool Start() override;
  void Stop() override;
  void SetViewControllerState(
      FlutterDesktopViewControllerState* state) override;

  // Update the absolute-motion clamp + scaling rectangle. Software
  // sinks may finalize the panel dimensions after construction (e.g.
  // DrmDumbSink uses the preferred mode, not the config geometry);
  // call this before Start() so the dispatch loop sees the final
  // values without an atomic.
  void SetViewport(int32_t width, int32_t height);

 private:
  void DispatchLoop();
  void DispatchLibinputEvents();
  void HandleEvent(libinput_event* ev);
  void HandlePointerMotion(libinput_event_pointer* p);
  void HandlePointerMotionAbsolute(libinput_event_pointer* p);
  void HandlePointerButton(libinput_event_pointer* p);
  void HandlePointerAxis(libinput_event_pointer* p);
  void HandleKeyboardKey(libinput_event_keyboard* k);

  // Dispatch a populated FlutterPointerEvent on the platform task
  // runner's strand. Copies the event by value into the lambda.
  void DispatchPointerEvent(const FlutterPointerEvent& pe);

  // Same for an arbitrary fixed-size batch (e.g. kAdd + kDown).
  void DispatchPointerEvents(const FlutterPointerEvent* pe, size_t count);

  // Snapshot of what we need from FlutterDesktopViewControllerState
  // without holding any reference past the call.
  struct EngineRef {
    void* engine{nullptr};           // FlutterEngine
    void* platform_runner{nullptr};  // TaskRunner
  };
  [[nodiscard]] EngineRef CurrentEngine() const;

  int32_t viewport_w_;
  int32_t viewport_h_;

  std::atomic<FlutterDesktopViewControllerState*> state_{nullptr};

  ::udev* udev_{nullptr};
  ::libinput* li_{nullptr};

  // xkb context / keymap / state for keysym translation. Uses the
  // system default RMLVO (xkbcommon's compiled-in defaults, then
  // XKB_DEFAULT_* env vars); good enough for the common case.
  xkb_context* xkb_ctx_{nullptr};
  xkb_keymap* xkb_keymap_{nullptr};
  xkb_state* xkb_state_{nullptr};

  std::thread thread_;
  std::atomic<bool> stop_{false};

  // Dispatch-thread state. Not atomic because only the worker thread
  // reads/writes; published to Flutter via the runner's strand.
  double pointer_x_{0.0};
  double pointer_y_{0.0};
  int64_t button_mask_{0};
  bool pointer_added_{false};
};

}  // namespace homescreen
