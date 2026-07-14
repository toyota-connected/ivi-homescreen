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

#include "backend/software/input/software_seat.h"

#include "backend/backend.h"
#include "backend/software/input/libinput_log_bridge.h"
#include "config/common.h"
#include "logging/logging.h"

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <pthread.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>

#include <asio/dispatch.hpp>

#include "backend/software/software_cursor.h"
#include "engine.h"
#include "libflutter_engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "task_runner.h"

extern void KeyCallback(FlutterDesktopViewControllerState* view_state,
                        bool released,
                        xkb_keysym_t keysym,
                        uint32_t xkb_scancode,
                        uint32_t modifiers);

namespace homescreen {

namespace {

// libinput's open/close hooks. With no libseat in the picture we just
// run open()/close() under the calling process's creds — operator
// puts themselves in the `input` group (or runs as root).
int OpenRestricted(const char* path, int flags, void* /*user_data*/) {
  const int fd = ::open(path, flags | O_CLOEXEC);
  if (fd < 0) {
    ihs::log::warn(
        "[SoftwareSeat] open('{}') failed: {} — add the user to the 'input' "
        "group (or run on an active VT for the logind uaccess ACL)",
        path, std::strerror(errno));
    return -errno;
  }
  ihs::log::debug("[SoftwareSeat] opened input device {}", path);
  return fd;
}

void CloseRestricted(int fd, void* /*user_data*/) {
  ::close(fd);
}

constexpr libinput_interface kLibinputInterface = {
    .open_restricted = OpenRestricted,
    .close_restricted = CloseRestricted,
};

// Forwards libinput's own diagnostics into ihs::log with a [libinput] tag,
// priority-mapped. Installed via libinput_log_set_handler so the "client bug:
// event processing lagging" marker (and device chatter at debug) lands in the
// structured log / DLT instead of libinput's default stderr sink.
void LibinputLogHandler(libinput* /*li*/,
                        const libinput_log_priority priority,
                        const char* format,
                        va_list args) {
  char buf[512];
  const int n = std::vsnprintf(buf, sizeof(buf), format, args);
  if (n <= 0) {
    return;
  }
  const auto len = std::min(static_cast<std::size_t>(n), sizeof(buf) - 1);
  std::string_view msg(buf, len);
  // libinput terminates its lines with '\n'; ihs::log adds its own framing.
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
    msg.remove_suffix(1);
  }
  switch (MapLibinputLogLevel(priority)) {
    case IHS_LEVEL_DEBUG:
      ihs::log::debug("[libinput] {}", msg);
      break;
    case IHS_LEVEL_INFO:
      ihs::log::info("[libinput] {}", msg);
      break;
    case IHS_LEVEL_ERROR:
      ihs::log::error("[libinput] {}", msg);
      break;
    default:
      ihs::log::warn("[libinput] {}", msg);
      break;
  }
}

// linux/input-event-codes BTN_* → Flutter button bit
int64_t MapMouseButton(const uint32_t button) {
  switch (button) {
    case BTN_LEFT:
      return kFlutterPointerButtonMousePrimary;
    case BTN_RIGHT:
      return kFlutterPointerButtonMouseSecondary;
    case BTN_MIDDLE:
      return kFlutterPointerButtonMouseMiddle;
    case BTN_SIDE:
      return kFlutterPointerButtonMouseBack;
    case BTN_EXTRA:
      return kFlutterPointerButtonMouseForward;
    default:
      return 0;
  }
}

uint64_t FlutterTimestampMicros() {
  // Flutter timestamps are µs since some epoch; FlutterEngineGetCurrentTime
  // is the engine's clock (CLOCK_MONOTONIC in ns).
  return LibFlutterEngine->GetCurrentTime() / 1000;
}

}  // namespace

SoftwareSeat::SoftwareSeat(const int32_t viewport_width,
                           const int32_t viewport_height)
    : viewport_w_(viewport_width), viewport_h_(viewport_height) {}

SoftwareSeat::~SoftwareSeat() {
  Stop();
  // keyboard_ / repeater_ are RAII (reset in Stop, after the dispatch thread
  // joins). xkb + the repeat timerfd are owned by those now.
  if (li_ != nullptr) {
    libinput_unref(li_);
    li_ = nullptr;
  }
  if (udev_ != nullptr) {
    udev_unref(udev_);
    udev_ = nullptr;
  }
}

bool SoftwareSeat::Start() {
  udev_ = udev_new();
  if (udev_ == nullptr) {
    ihs::log::error("[SoftwareSeat] udev_new failed");
    return false;
  }
  li_ = libinput_udev_create_context(&kLibinputInterface, this, udev_);
  if (li_ == nullptr) {
    ihs::log::error("[SoftwareSeat] libinput_udev_create_context failed");
    return false;
  }
  // Route libinput's own diagnostics into ihs::log. INFO priority is requested
  // explicitly so the "client bug: event processing lagging" marker is emitted
  // regardless of libinput's build-time default (which suppresses it below
  // ERROR on some versions).
  libinput_log_set_handler(li_, LibinputLogHandler);
  libinput_log_set_priority(li_, LIBINPUT_LOG_PRIORITY_INFO);
  if (libinput_udev_assign_seat(li_, "seat0") != 0) {
    ihs::log::error("[SoftwareSeat] libinput_udev_assign_seat('seat0') failed");
    return false;
  }

  // Shared keyboard translation + auto-repeat — the same input::XkbKeyboard /
  // input::KeyRepeater path the DRM seats use, so keyboard behavior (including
  // 25 Hz repeat with per-tick sym re-resolution + modifiers) is identical
  // across backends. xkb picks up RMLVO from XKB_DEFAULT_* / built-in defaults.
  keyboard_ = input::XkbKeyboard::Create();
  if (!keyboard_) {
    ihs::log::error("[SoftwareSeat] XkbKeyboard::Create failed");
    return false;
  }
  repeater_ = input::KeyRepeater::Create(keyboard_.get());
  if (repeater_) {
    // A repeat tick re-resolves the held key against the current xkb state and
    // re-dispatches it as a press with the live modifier mask.
    repeater_->SetHandler([this](uint32_t evdev_keycode, xkb_keysym_t sym,
                                 const std::string& /*utf8*/) {
      if (auto* s = state_.load(std::memory_order_acquire); s != nullptr) {
        KeyCallback(s, /*released=*/false, sym, evdev_keycode + 8,
                    keyboard_->Modifiers());
      }
    });
  } else {
    ihs::log::warn("[SoftwareSeat] key repeat unavailable (timerfd_create)");
  }

  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this]() { DispatchLoop(); });
  ihs::log::info("[SoftwareSeat] started ({}x{} viewport)", viewport_w_,
                 viewport_h_);
  return true;
}

void SoftwareSeat::Stop() {
  stop_.store(true, std::memory_order_release);
  // Store-then-wake: the dispatch thread is blocked in poll(-1); Wake() makes
  // the waker fd readable so the poll returns and the loop re-checks stop_.
  waker_.Wake();
  if (thread_.joinable()) {
    thread_.join();
  }
  // Safe to drop the keyboard + repeater now the dispatch thread has joined
  // (it is their only user). RAII closes the xkb state + the repeat timerfd.
  repeater_.reset();
  keyboard_.reset();
}

void SoftwareSeat::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  state_.store(state, std::memory_order_release);
}

void SoftwareSeat::SetViewport(const int32_t width, const int32_t height) {
  viewport_w_ = width;
  viewport_h_ = height;
}

void SoftwareSeat::SetCursor(std::shared_ptr<SoftwareCursor> cursor) {
  cursor_ = std::move(cursor);
}

void SoftwareSeat::NotifyCursorMoved() {
  if (!cursor_) {
    return;
  }
  static bool logged_first = false;
  if (!logged_first) {
    logged_first = true;
    ihs::log::debug(
        "[SoftwareSeat] cursor tracking pointer (first motion {},{})",
        static_cast<int>(pointer_x_), static_cast<int>(pointer_y_));
  }
  // pointer_x_/y_ are in viewport (== framebuffer mode) pixels, the same space
  // the cursor blends into.
  cursor_->SetPosition(static_cast<int32_t>(pointer_x_),
                       static_cast<int32_t>(pointer_y_));
  // Flutter only presents dirty frames, so without a nudge the cursor would
  // freeze on an idle UI. ScheduleFrame is thread-safe; the dumb sink then
  // repaints with the cursor at its new position.
  if (const EngineRef e = CurrentEngine(); e.engine != nullptr) {
    LibFlutterEngine->ScheduleFrame(
        static_cast<FLUTTER_API_SYMBOL(FlutterEngine)>(e.engine));
  }
}

void SoftwareSeat::DispatchLoop() {
  pthread_setname_np(pthread_self(), "li-sw-dispatch");

  // Event-driven: block in poll() with no timeout until libinput has events,
  // the key-repeat timerfd fires, or waker_ is signaled. stop_ is noticed
  // without a poll cap because Stop() stores the flag then Wake()s the waker,
  // so the poll returns and the loop re-checks the flag at the top. Degraded
  // mode (waker fd unavailable, e.g. eventfd() failed) falls back to a 16 ms
  // cap — roughly one vblank — so Stop() is still observed promptly.
  const int li_fd = libinput_get_fd(li_);
  const int repeat_fd = repeater_ ? repeater_->fd() : -1;
  const int wake_fd = waker_.fd();

  pollfd fds[3];
  nfds_t nfds = 0;
  const int li_idx = static_cast<int>(nfds);
  fds[nfds++] = pollfd{li_fd, static_cast<short>(POLLIN), 0};
  const int repeat_idx = repeat_fd >= 0 ? static_cast<int>(nfds) : -1;
  if (repeat_fd >= 0) {
    fds[nfds++] = pollfd{repeat_fd, static_cast<short>(POLLIN), 0};
  }
  const int wake_idx = wake_fd >= 0 ? static_cast<int>(nfds) : -1;
  if (wake_fd >= 0) {
    fds[nfds++] = pollfd{wake_fd, static_cast<short>(POLLIN), 0};
  }
  const int timeout_ms = wake_fd >= 0 ? -1 : 16;

  while (!stop_.load(std::memory_order_acquire)) {
    const int rc = ::poll(fds, nfds, timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      ihs::log::error("[SoftwareSeat] poll: {}", std::strerror(errno));
      break;
    }
    // A waker wake is a pure "re-check your flags" nudge: drain and loop so the
    // stop_ test at the top runs. (rc == 0 only in degraded mode; same effect.)
    if (wake_idx >= 0 && (fds[wake_idx].revents & POLLIN) != 0) {
      waker_.Drain();
      continue;
    }
    if ((fds[li_idx].revents & POLLIN) != 0) {
      DispatchLibinputEvents();
    }
    if (repeat_idx >= 0 && (fds[repeat_idx].revents & POLLIN) != 0) {
      repeater_->Dispatch();
    }
  }
}

void SoftwareSeat::DispatchLibinputEvents() {
  if (libinput_dispatch(li_) != 0) {
    // Non-fatal: libinput logs the underlying error itself.
    return;
  }
  while (libinput_event* ev = libinput_get_event(li_)) {
    HandleEvent(ev);
    libinput_event_destroy(ev);
  }
}

void SoftwareSeat::HandleEvent(libinput_event* ev) {
  switch (libinput_event_get_type(ev)) {
    case LIBINPUT_EVENT_POINTER_MOTION:
      HandlePointerMotion(libinput_event_get_pointer_event(ev));
      break;
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
      HandlePointerMotionAbsolute(libinput_event_get_pointer_event(ev));
      break;
    case LIBINPUT_EVENT_POINTER_BUTTON:
      HandlePointerButton(libinput_event_get_pointer_event(ev));
      break;
    case LIBINPUT_EVENT_POINTER_AXIS:
      HandlePointerAxis(libinput_event_get_pointer_event(ev));
      break;
    case LIBINPUT_EVENT_KEYBOARD_KEY:
      HandleKeyboardKey(libinput_event_get_keyboard_event(ev));
      break;
    case LIBINPUT_EVENT_TOUCH_DOWN:
      HandleTouchDown(libinput_event_get_touch_event(ev));
      break;
    case LIBINPUT_EVENT_TOUCH_UP:
      HandleTouchUp(libinput_event_get_touch_event(ev));
      break;
    case LIBINPUT_EVENT_TOUCH_MOTION:
      HandleTouchMotion(libinput_event_get_touch_event(ev));
      break;
    case LIBINPUT_EVENT_TOUCH_CANCEL:
      HandleTouchCancel(libinput_event_get_touch_event(ev));
      break;
    case LIBINPUT_EVENT_TOUCH_FRAME:
      // Frame marker: everything since the previous marker is one hardware
      // scan — hand it to the engine as one batch (one strand post, one
      // FlutterEngineSendPointerEvent, one UI-thread task) instead of one
      // submission per contact. A 10-finger drag at a 250 Hz scan rate is
      // 250 posts/s instead of 2500/s.
      FlushTouchBatch();
      break;
    default:
      // No-op alongside gesture / switch events that aren't wired yet.
      break;
  }
}

void SoftwareSeat::HandlePointerMotion(libinput_event_pointer* p) {
  // Guard against degenerate viewport — a sink that hasn't reported
  // its mode yet (or a CI run with width/height=0) would otherwise
  // hit std::clamp(x, 0.0, -1.0), which is UB.
  if (viewport_w_ <= 0 || viewport_h_ <= 0) {
    return;
  }
  const double dx = libinput_event_pointer_get_dx(p);
  const double dy = libinput_event_pointer_get_dy(p);
  pointer_x_ =
      std::clamp(pointer_x_ + dx, 0.0, static_cast<double>(viewport_w_ - 1));
  pointer_y_ =
      std::clamp(pointer_y_ + dy, 0.0, static_cast<double>(viewport_h_ - 1));

  FlutterPointerEvent batch[2];
  size_t count = 0;
  const uint64_t ts = FlutterTimestampMicros();
  if (!pointer_added_) {
    batch[count] = FlutterPointerEvent{};
    batch[count].struct_size = sizeof(FlutterPointerEvent);
    batch[count].phase = kAdd;
    batch[count].timestamp = ts;
    batch[count].x = pointer_x_;
    batch[count].y = pointer_y_;
    batch[count].device = 0;
    batch[count].device_kind = kFlutterPointerDeviceKindMouse;
    pointer_added_ = true;
    ++count;
  }
  batch[count] = FlutterPointerEvent{};
  batch[count].struct_size = sizeof(FlutterPointerEvent);
  batch[count].phase = button_mask_ != 0 ? kMove : kHover;
  batch[count].timestamp = ts;
  batch[count].x = pointer_x_;
  batch[count].y = pointer_y_;
  batch[count].device = 0;
  batch[count].device_kind = kFlutterPointerDeviceKindMouse;
  batch[count].buttons = button_mask_;
  ++count;

  NotifyCursorMoved();
  DispatchPointerEvents(batch, count);
}

void SoftwareSeat::HandlePointerMotionAbsolute(libinput_event_pointer* p) {
  if (viewport_w_ <= 0 || viewport_h_ <= 0) {
    return;
  }
  pointer_x_ = libinput_event_pointer_get_absolute_x_transformed(
      p, static_cast<uint32_t>(viewport_w_));
  pointer_y_ = libinput_event_pointer_get_absolute_y_transformed(
      p, static_cast<uint32_t>(viewport_h_));

  FlutterPointerEvent batch[2];
  size_t count = 0;
  const uint64_t ts = FlutterTimestampMicros();
  if (!pointer_added_) {
    batch[count] = FlutterPointerEvent{};
    batch[count].struct_size = sizeof(FlutterPointerEvent);
    batch[count].phase = kAdd;
    batch[count].timestamp = ts;
    batch[count].x = pointer_x_;
    batch[count].y = pointer_y_;
    batch[count].device = 0;
    batch[count].device_kind = kFlutterPointerDeviceKindMouse;
    pointer_added_ = true;
    ++count;
  }
  batch[count] = FlutterPointerEvent{};
  batch[count].struct_size = sizeof(FlutterPointerEvent);
  batch[count].phase = button_mask_ != 0 ? kMove : kHover;
  batch[count].timestamp = ts;
  batch[count].x = pointer_x_;
  batch[count].y = pointer_y_;
  batch[count].device = 0;
  batch[count].device_kind = kFlutterPointerDeviceKindMouse;
  batch[count].buttons = button_mask_;
  ++count;

  NotifyCursorMoved();
  DispatchPointerEvents(batch, count);
}

void SoftwareSeat::HandlePointerButton(libinput_event_pointer* p) {
  const uint32_t button = libinput_event_pointer_get_button(p);
  const auto state = libinput_event_pointer_get_button_state(p);
  const int64_t bit = MapMouseButton(button);
  if (bit == 0) {
    return;
  }
  const int64_t prev_mask = button_mask_;
  const bool pressed = state == LIBINPUT_BUTTON_STATE_PRESSED;
  if (pressed) {
    button_mask_ |= bit;
  } else {
    button_mask_ &= ~bit;
  }

  FlutterPointerPhase phase;
  if (pressed) {
    phase = (prev_mask == 0) ? kDown : kMove;
  } else {
    phase = (button_mask_ == 0) ? kUp : kMove;
  }

  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = phase;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = pointer_x_;
  pe.y = pointer_y_;
  pe.device = 0;
  pe.device_kind = kFlutterPointerDeviceKindMouse;
  pe.buttons = button_mask_;
  DispatchPointerEvent(pe);
}

void SoftwareSeat::HandlePointerAxis(libinput_event_pointer* p) {
  // libinput separates horizontal and vertical scroll axes. Both
  // arrive in the same event for a typical wheel/touchpad scroll.
  double dx = 0.0;
  double dy = 0.0;
  if (libinput_event_pointer_has_axis(
          p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) != 0) {
    dx = libinput_event_pointer_get_axis_value(
        p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
  }
  if (libinput_event_pointer_has_axis(
          p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) != 0) {
    dy = libinput_event_pointer_get_axis_value(
        p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
  }
  if (dx == 0.0 && dy == 0.0) {
    return;
  }
  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = button_mask_ != 0 ? kMove : kHover;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = pointer_x_;
  pe.y = pointer_y_;
  pe.device = 0;
  pe.device_kind = kFlutterPointerDeviceKindMouse;
  pe.buttons = button_mask_;
  pe.signal_kind = kFlutterPointerSignalKindScroll;
  pe.scroll_delta_x = dx;
  pe.scroll_delta_y = dy;
  DispatchPointerEvent(pe);
}

void SoftwareSeat::HandleKeyboardKey(libinput_event_keyboard* k) {
  const uint32_t key = libinput_event_keyboard_get_key(k);
  const bool pressed =
      libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
  // The shared keyboard resolves the keysym and advances modifier state (it
  // applies the universal evdev->xkb +8 offset internally).
  const xkb_keysym_t keysym = keyboard_->ProcessKey(key, pressed);
  if (auto* s = state_.load(std::memory_order_acquire); s != nullptr) {
    KeyCallback(s, !pressed, keysym, key + 8, keyboard_->Modifiers());
  }
  // Feed the shared repeater — it owns per-key eligibility, most-recent-wins
  // arming, and the timerfd polled in DispatchLoop.
  if (repeater_) {
    repeater_->OnKey(key, pressed);
  }
}

void SoftwareSeat::HandleTouchDown(libinput_event_touch* t) {
  if (viewport_w_ <= 0 || viewport_h_ <= 0) {
    return;
  }
  // Touch input → hide the mouse cursor (it reappears on the next pointer
  // motion). Matches typical desktop behaviour.
  if (cursor_) {
    cursor_->SetVisible(false);
  }
  const int32_t slot = libinput_event_touch_get_seat_slot(t);
  if (slot < 0 || static_cast<size_t>(slot) >= kMaxTouchSlots) {
    return;
  }
  auto& s = touch_[static_cast<size_t>(slot)];
  s.x = libinput_event_touch_get_x_transformed(
      t, static_cast<uint32_t>(viewport_w_));
  s.y = libinput_event_touch_get_y_transformed(
      t, static_cast<uint32_t>(viewport_h_));
  s.down = true;

  // Each touch slot gets its own Flutter device id so multi-touch
  // sequences don't collide. kAdd + kDown in a single batch — Flutter
  // requires kAdd before any phase on a previously-unseen device.
  FlutterPointerEvent batch[2];
  const uint64_t ts = FlutterTimestampMicros();
  batch[0] = FlutterPointerEvent{};
  batch[0].struct_size = sizeof(FlutterPointerEvent);
  batch[0].phase = kAdd;
  batch[0].timestamp = ts;
  batch[0].x = s.x;
  batch[0].y = s.y;
  batch[0].device = slot;
  batch[0].device_kind = kFlutterPointerDeviceKindTouch;
  batch[1] = FlutterPointerEvent{};
  batch[1].struct_size = sizeof(FlutterPointerEvent);
  batch[1].phase = kDown;
  batch[1].timestamp = ts;
  batch[1].x = s.x;
  batch[1].y = s.y;
  batch[1].device = slot;
  batch[1].device_kind = kFlutterPointerDeviceKindTouch;
  QueueTouchEvent(batch[0]);
  QueueTouchEvent(batch[1]);
}

void SoftwareSeat::HandleTouchMotion(libinput_event_touch* t) {
  if (viewport_w_ <= 0 || viewport_h_ <= 0) {
    return;
  }
  const int32_t slot = libinput_event_touch_get_seat_slot(t);
  if (slot < 0 || static_cast<size_t>(slot) >= kMaxTouchSlots) {
    return;
  }
  auto& s = touch_[static_cast<size_t>(slot)];
  if (!s.down) {
    return;  // motion without prior down — drop, don't fabricate state
  }
  s.x = libinput_event_touch_get_x_transformed(
      t, static_cast<uint32_t>(viewport_w_));
  s.y = libinput_event_touch_get_y_transformed(
      t, static_cast<uint32_t>(viewport_h_));

  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = kMove;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = s.x;
  pe.y = s.y;
  pe.device = slot;
  pe.device_kind = kFlutterPointerDeviceKindTouch;
  QueueTouchEvent(pe);
}

void SoftwareSeat::HandleTouchUp(libinput_event_touch* t) {
  const int32_t slot = libinput_event_touch_get_seat_slot(t);
  if (slot < 0 || static_cast<size_t>(slot) >= kMaxTouchSlots) {
    return;
  }
  auto& s = touch_[static_cast<size_t>(slot)];
  if (!s.down) {
    return;
  }
  s.down = false;

  // kUp + kRemove paired so Flutter retires the device id. Same
  // (x, y) as the most recent motion — libinput's TOUCH_UP doesn't
  // carry coordinates, so we keep the last-known position.
  FlutterPointerEvent batch[2];
  const uint64_t ts = FlutterTimestampMicros();
  batch[0] = FlutterPointerEvent{};
  batch[0].struct_size = sizeof(FlutterPointerEvent);
  batch[0].phase = kUp;
  batch[0].timestamp = ts;
  batch[0].x = s.x;
  batch[0].y = s.y;
  batch[0].device = slot;
  batch[0].device_kind = kFlutterPointerDeviceKindTouch;
  batch[1] = FlutterPointerEvent{};
  batch[1].struct_size = sizeof(FlutterPointerEvent);
  batch[1].phase = kRemove;
  batch[1].timestamp = ts;
  batch[1].x = s.x;
  batch[1].y = s.y;
  batch[1].device = slot;
  batch[1].device_kind = kFlutterPointerDeviceKindTouch;
  QueueTouchEvent(batch[0]);
  QueueTouchEvent(batch[1]);
}

void SoftwareSeat::HandleTouchCancel(libinput_event_touch* t) {
  const int32_t slot = libinput_event_touch_get_seat_slot(t);
  if (slot < 0 || static_cast<size_t>(slot) >= kMaxTouchSlots) {
    return;
  }
  auto& s = touch_[static_cast<size_t>(slot)];
  if (!s.down) {
    return;
  }
  s.down = false;

  FlutterPointerEvent batch[2];
  const uint64_t ts = FlutterTimestampMicros();
  batch[0] = FlutterPointerEvent{};
  batch[0].struct_size = sizeof(FlutterPointerEvent);
  batch[0].phase = kCancel;
  batch[0].timestamp = ts;
  batch[0].x = s.x;
  batch[0].y = s.y;
  batch[0].device = slot;
  batch[0].device_kind = kFlutterPointerDeviceKindTouch;
  batch[1] = FlutterPointerEvent{};
  batch[1].struct_size = sizeof(FlutterPointerEvent);
  batch[1].phase = kRemove;
  batch[1].timestamp = ts;
  batch[1].x = s.x;
  batch[1].y = s.y;
  batch[1].device = slot;
  batch[1].device_kind = kFlutterPointerDeviceKindTouch;
  QueueTouchEvent(batch[0]);
  QueueTouchEvent(batch[1]);
  // Not every stack guarantees a TOUCH_FRAME after cancel; flush now so the
  // engine's per-device state is terminated promptly.
  FlushTouchBatch();
}

void SoftwareSeat::QueueTouchEvent(const FlutterPointerEvent& pe) {
  touch_batch_.push_back(pe);
  if (touch_batch_.size() >= static_cast<size_t>(kMaxPointerEvent)) {
    FlushTouchBatch();
  }
}

void SoftwareSeat::FlushTouchBatch() {
  if (touch_batch_.empty()) {
    return;
  }
  // One timestamp for the whole frame: the contacts in a touch frame are
  // logically simultaneous (one hardware scan), and a shared timestamp keeps
  // the framework's pointer resampler from interpolating skew between fingers
  // that moved together.
  const uint64_t ts = FlutterTimestampMicros();
  for (auto& e : touch_batch_) {
    e.timestamp = ts;
  }
  DispatchPointerEvents(touch_batch_.data(), touch_batch_.size());
  touch_batch_.clear();
}

SoftwareSeat::EngineRef SoftwareSeat::CurrentEngine() const {
  auto* s = state_.load(std::memory_order_acquire);
  if (s == nullptr || s->engine == nullptr) {
    return {};
  }
  EngineRef out;
  out.engine = static_cast<void*>(s->engine->GetFlutterEngine());
  out.platform_runner = static_cast<void*>(s->engine->GetPlatformTaskRunner());
  if (auto* backend = s->engine->GetBackend(); backend != nullptr) {
    out.m2p = static_cast<void*>(backend->GetMotionToPhoton());
  }
  return out;
}

void SoftwareSeat::DispatchPointerEvent(const FlutterPointerEvent& pe) {
  DispatchPointerEvents(&pe, 1);
}

void SoftwareSeat::DispatchPointerEvents(const FlutterPointerEvent* pe,
                                         const size_t count) {
  const EngineRef ref = CurrentEngine();
  if (ref.engine == nullptr || ref.platform_runner == nullptr) {
    return;
  }
  auto* runner = static_cast<TaskRunner*>(ref.platform_runner);
  auto* strand = runner->GetStrandContext();
  if (strand == nullptr) {
    return;
  }
  auto* engine = static_cast<FLUTTER_API_SYMBOL(FlutterEngine)>(ref.engine);
  // Motion-to-photon input endpoint (IVI_M2P_PROFILE): the batch shares one
  // engine-clock timestamp; record it on the strand so it joins the sink's
  // flip-path RecordPresent on the same thread. Null unless profiling.
  auto* m2p = static_cast<profiling::MotionToPhoton*>(ref.m2p);
  const uint64_t input_us = count > 0 ? pe[count - 1].timestamp : 0;
  std::vector<FlutterPointerEvent> events(pe, pe + count);
  asio::dispatch(*strand, [engine, events = std::move(events), m2p,
                           input_us]() {
    if (m2p != nullptr && input_us != 0) {
      m2p->RecordInput(input_us * 1000ULL);
    }
    LibFlutterEngine->SendPointerEvent(engine, events.data(), events.size());
  });
}

}  // namespace homescreen
