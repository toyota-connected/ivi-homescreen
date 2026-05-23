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

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

#include <asio/dispatch.hpp>

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
  return fd < 0 ? -errno : fd;
}

void CloseRestricted(int fd, void* /*user_data*/) {
  ::close(fd);
}

constexpr libinput_interface kLibinputInterface = {
    .open_restricted = OpenRestricted,
    .close_restricted = CloseRestricted,
};

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
  if (xkb_state_ != nullptr) {
    xkb_state_unref(xkb_state_);
    xkb_state_ = nullptr;
  }
  if (xkb_keymap_ != nullptr) {
    xkb_keymap_unref(xkb_keymap_);
    xkb_keymap_ = nullptr;
  }
  if (xkb_ctx_ != nullptr) {
    xkb_context_unref(xkb_ctx_);
    xkb_ctx_ = nullptr;
  }
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
    spdlog::error("[SoftwareSeat] udev_new failed");
    return false;
  }
  li_ = libinput_udev_create_context(&kLibinputInterface, this, udev_);
  if (li_ == nullptr) {
    spdlog::error("[SoftwareSeat] libinput_udev_create_context failed");
    return false;
  }
  if (libinput_udev_assign_seat(li_, "seat0") != 0) {
    spdlog::error("[SoftwareSeat] libinput_udev_assign_seat('seat0') failed");
    return false;
  }

  // xkb defaults — rules/model/layout/variant/options all picked up
  // from environment (XKB_DEFAULT_RULES etc.) or built-in defaults.
  xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (xkb_ctx_ == nullptr) {
    spdlog::error("[SoftwareSeat] xkb_context_new failed");
    return false;
  }
  xkb_keymap_ =
      xkb_keymap_new_from_names(xkb_ctx_, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (xkb_keymap_ == nullptr) {
    spdlog::error("[SoftwareSeat] xkb_keymap_new_from_names failed");
    return false;
  }
  xkb_state_ = xkb_state_new(xkb_keymap_);
  if (xkb_state_ == nullptr) {
    spdlog::error("[SoftwareSeat] xkb_state_new failed");
    return false;
  }

  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this]() { DispatchLoop(); });
  spdlog::info("[SoftwareSeat] started ({}x{} viewport)", viewport_w_,
               viewport_h_);
  return true;
}

void SoftwareSeat::Stop() {
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void SoftwareSeat::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  state_.store(state, std::memory_order_release);
}

void SoftwareSeat::SetViewport(const int32_t width, const int32_t height) {
  viewport_w_ = width;
  viewport_h_ = height;
}

void SoftwareSeat::DispatchLoop() {
  // libinput's fd is the readiness gate; we still need to call
  // libinput_dispatch() before draining events. Short poll timeout
  // gives Stop() a worst-case wakeup latency comparable to one
  // vblank.
  const int li_fd = libinput_get_fd(li_);
  pollfd pfd{li_fd, POLLIN, 0};
  while (!stop_.load(std::memory_order_acquire)) {
    const int rc = ::poll(&pfd, 1, 16);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[SoftwareSeat] poll: {}", std::strerror(errno));
      break;
    }
    if (rc > 0 && (pfd.revents & POLLIN) != 0) {
      DispatchLibinputEvents();
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
    default:
      // TOUCH_FRAME is a batch-boundary marker; we dispatch events as
      // they arrive, so no accumulation to flush. Falls through into
      // the default no-op alongside gesture / switch events that
      // aren't wired yet.
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
  const auto state = libinput_event_keyboard_get_key_state(k);
  // evdev keycodes are offset by 8 from xkb's scancode space; this is
  // the universal Linux convention.
  const uint32_t xkb_scancode = key + 8;
  const xkb_keysym_t keysym =
      xkb_state_key_get_one_sym(xkb_state_, xkb_scancode);
  xkb_state_update_key(
      xkb_state_, xkb_scancode,
      state == LIBINPUT_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
  auto* s = state_.load(std::memory_order_acquire);
  if (s == nullptr) {
    return;
  }
  KeyCallback(s, state == LIBINPUT_KEY_STATE_RELEASED, keysym, xkb_scancode, 0);
}

void SoftwareSeat::HandleTouchDown(libinput_event_touch* t) {
  if (viewport_w_ <= 0 || viewport_h_ <= 0) {
    return;
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
  DispatchPointerEvents(batch, 2);
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
  DispatchPointerEvent(pe);
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
  DispatchPointerEvents(batch, 2);
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
  DispatchPointerEvents(batch, 2);
}

SoftwareSeat::EngineRef SoftwareSeat::CurrentEngine() const {
  auto* s = state_.load(std::memory_order_acquire);
  if (s == nullptr || s->engine == nullptr) {
    return {};
  }
  EngineRef out;
  out.engine = static_cast<void*>(s->engine->GetFlutterEngine());
  out.platform_runner = static_cast<void*>(s->engine->GetPlatformTaskRunner());
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
  std::vector<FlutterPointerEvent> events(pe, pe + count);
  asio::dispatch(*strand, [engine, events = std::move(events)]() {
    LibFlutterEngine->SendPointerEvent(engine, events.data(), events.size());
  });
}

}  // namespace homescreen
