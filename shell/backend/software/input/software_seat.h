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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <xkbcommon/xkbcommon.h>

#include <shell/platform/embedder/embedder.h>

#include "input/iseat.h"
#include "input/key_repeater.h"
#include "input/wake_event_fd.h"
#include "input/xkb_keyboard.h"

class SoftwareCursor;

struct libinput;
struct libinput_event;
struct libinput_event_pointer;
struct libinput_event_keyboard;
struct libinput_event_touch;
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

  // Install the shared software cursor. On pointer motion the seat updates the
  // cursor position and schedules a frame, so the cursor tracks the mouse even
  // when the UI is otherwise idle. Null leaves the cursor unmanaged.
  void SetCursor(std::shared_ptr<SoftwareCursor> cursor);

 private:
  // Push the current pointer position to the cursor and schedule a frame so the
  // dumb sink repaints with the cursor at its new spot. No-op without a cursor.
  void NotifyCursorMoved();

  void DispatchLoop();
  void DispatchLibinputEvents();
  void HandleEvent(libinput_event* ev);
  void HandlePointerMotion(libinput_event_pointer* p);
  void HandlePointerMotionAbsolute(libinput_event_pointer* p);
  void HandlePointerButton(libinput_event_pointer* p);
  void HandlePointerAxis(libinput_event_pointer* p);
  void HandleKeyboardKey(libinput_event_keyboard* k);
  void HandleTouchDown(libinput_event_touch* t);
  void HandleTouchUp(libinput_event_touch* t);
  void HandleTouchMotion(libinput_event_touch* t);
  void HandleTouchCancel(libinput_event_touch* t);

  // Hand everything accumulated since the last LIBINPUT_EVENT_TOUCH_FRAME to
  // the engine as one batch: one shared timestamp, one strand post, one
  // FlutterEngineSendPointerEvent (= one UI-thread task in the engine) per
  // hardware scan instead of one per contact. No-op when nothing is pending.
  void FlushTouchBatch();

  // Append an event to the pending touch frame. The timestamp is stamped for
  // the whole group in FlushTouchBatch — contacts within one frame are
  // logically simultaneous. Flushes early as a backstop if the batch would
  // exceed kMaxPointerEvent (a runaway stream without frame markers must not
  // grow unbounded).
  void QueueTouchEvent(const FlutterPointerEvent& pe);

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
    void* m2p{nullptr};              // profiling::MotionToPhoton (null unless
                                     // IVI_M2P_PROFILE)
  };
  [[nodiscard]] EngineRef CurrentEngine() const;

  int32_t viewport_w_;
  int32_t viewport_h_;

  std::atomic<FlutterDesktopViewControllerState*> state_{nullptr};

  ::udev* udev_{nullptr};
  ::libinput* li_{nullptr};

  // Shared keyboard translation + auto-repeat (input::XkbKeyboard /
  // input::KeyRepeater in shell/input/), the same path the DRM seats use, so
  // keyboard behavior is identical across backends. repeater_ may be null if
  // its timerfd couldn't be created (key repeat then disabled). Both live on,
  // and are only touched by, the dispatch thread.
  std::unique_ptr<input::XkbKeyboard> keyboard_;
  std::unique_ptr<input::KeyRepeater> repeater_;

  std::thread thread_;
  std::atomic<bool> stop_{false};

  // Breaks DispatchLoop out of its infinite poll() when stop_ is set. Written
  // (Wake) from Stop() on the caller's thread; drained on the dispatch thread.
  input::WakeEventFd waker_;

  // Dispatch-thread state. Not atomic because only the worker thread
  // reads/writes; published to Flutter via the runner's strand.
  double pointer_x_{0.0};
  double pointer_y_{0.0};
  int64_t button_mask_{0};
  bool pointer_added_{false};

  // Shared software cursor (owned by SoftwareDisplay). Written here on the
  // dispatch thread; read on the rasterizer thread in DrmDumbSink::Present.
  // Its position/visibility are atomics, so no extra locking. Null = disabled.
  std::shared_ptr<SoftwareCursor> cursor_;

  // Per-slot multi-touch state. libinput uses slot indices for
  // multi-touch; we mirror the slot directly as Flutter's `device`
  // id so each finger gets its own kAdd / kDown / kMove / kUp /
  // kRemove sequence. Slots that touchscreen drivers report
  // out-of-range are dropped.
  static constexpr size_t kMaxTouchSlots = 16;
  struct TouchSlot {
    bool down{false};
    double x{0.0};
    double y{0.0};
  };
  std::array<TouchSlot, kMaxTouchSlots> touch_{};

  // Touch events accumulated since the last LIBINPUT_EVENT_TOUCH_FRAME.
  // libinput groups the per-slot updates of one hardware scan between frame
  // markers; see FlushTouchBatch. Touched only on the libinput worker thread.
  std::vector<FlutterPointerEvent> touch_batch_;
};

}  // namespace homescreen
