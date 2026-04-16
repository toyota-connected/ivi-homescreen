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

#include "input/drm_seat.h"

#include <linux/input-event-codes.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <variant>

#include "engine.h"
#include "input/key_mapping.h"
#include "libflutter_engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "shell/platform/homescreen/flutter_desktop_view_controller_state.h"

namespace homescreen {

namespace {

constexpr int kPollTimeoutMs = 100;

size_t FlutterTimestampMicros() {
  return static_cast<size_t>(LibFlutterEngine->GetCurrentTime() / 1000);
}

int64_t MapMouseButton(uint32_t evdev_button) {
  switch (evdev_button) {
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

}  // namespace

DrmSeat::DrmSeat(int32_t viewport_width, int32_t viewport_height)
    : viewport_w_(viewport_width), viewport_h_(viewport_height) {
  // Start the pointer in the middle of the viewport so the first hover is
  // visible before the user moves the mouse.
  pointer_x_ = viewport_w_ / 2.0;
  pointer_y_ = viewport_h_ / 2.0;
}

DrmSeat::~DrmSeat() {
  Stop();
}

void DrmSeat::SetViewControllerState(FlutterDesktopViewControllerState* state) {
  state_.store(state, std::memory_order_release);
}

FLUTTER_API_SYMBOL(FlutterEngine) DrmSeat::CurrentEngine() const {
  auto* state = state_.load(std::memory_order_acquire);
  if (!state || !state->engine) {
    return nullptr;
  }
  return state->engine->GetFlutterEngine();
}

bool DrmSeat::Start() {
  if (seat_) {
    return true;
  }

  auto opened = drm::input::Seat::open();
  if (!opened) {
    spdlog::error("[DrmSeat] libinput seat open failed: {}",
                  opened.error().message());
    return false;
  }
  seat_ = std::make_unique<drm::input::Seat>(std::move(*opened));
  seat_->set_event_handler(
      [this](const drm::input::InputEvent& ev) { HandleEvent(ev); });

  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this] { DispatchLoop(); });
  spdlog::info("[DrmSeat] started");
  return true;
}

void DrmSeat::Stop() {
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
  seat_.reset();
}

void DrmSeat::DispatchLoop() {
  const int fd = seat_->fd();
  while (!stop_.load(std::memory_order_acquire)) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int r = poll(&pfd, 1, kPollTimeoutMs);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[DrmSeat] poll: {}", std::strerror(errno));
      break;
    }
    if (r > 0 && (pfd.revents & POLLIN)) {
      if (auto ok = seat_->dispatch(); !ok) {
        spdlog::warn("[DrmSeat] dispatch: {}", ok.error().message());
      }
    }
  }
}

void DrmSeat::HandleEvent(const drm::input::InputEvent& ev) {
  std::visit(
      [this](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, drm::input::KeyboardEvent>) {
          HandleKeyboard(e);
        } else if constexpr (std::is_same_v<T, drm::input::PointerEvent>) {
          std::visit(
              [this](auto&& pe) {
                using P = std::decay_t<decltype(pe)>;
                if constexpr (std::is_same_v<P,
                                             drm::input::PointerMotionEvent>) {
                  HandlePointerMotion(pe);
                } else if constexpr (std::is_same_v<
                                         P, drm::input::PointerButtonEvent>) {
                  HandlePointerButton(pe);
                } else if constexpr (std::is_same_v<
                                         P, drm::input::PointerAxisEvent>) {
                  HandlePointerAxis(pe);
                }
              },
              e);
        } else if constexpr (std::is_same_v<T, drm::input::TouchEvent>) {
          HandleTouch(e);
        }
        // SwitchEvent (lid / tablet-mode) is intentionally dropped.
      },
      ev);
}

void DrmSeat::HandleKeyboard(const drm::input::KeyboardEvent& ev) {
  auto engine = CurrentEngine();
  if (!engine) {
    return;
  }

  FlutterKeyEvent ke{};
  ke.struct_size = sizeof(FlutterKeyEvent);
  ke.timestamp = static_cast<double>(FlutterTimestampMicros());
  ke.type = ev.pressed ? kFlutterKeyEventTypeDown : kFlutterKeyEventTypeUp;
  ke.physical = keys::EvdevToPhysical(ev.key);
  ke.logical = keys::DeriveLogicalKey(ev.utf8, ev.sym);
  ke.character = ev.utf8[0] ? ev.utf8 : nullptr;
  ke.synthesized = false;
  ke.device_type = kFlutterKeyEventDeviceTypeKeyboard;

  LibFlutterEngine->SendKeyEvent(engine, &ke, nullptr, nullptr);
}

void DrmSeat::HandlePointerMotion(const drm::input::PointerMotionEvent& ev) {
  auto engine = CurrentEngine();
  if (!engine) {
    return;
  }

  pointer_x_ =
      std::clamp(pointer_x_ + ev.dx, 0.0, static_cast<double>(viewport_w_ - 1));
  pointer_y_ =
      std::clamp(pointer_y_ + ev.dy, 0.0, static_cast<double>(viewport_h_ - 1));

  FlutterPointerEvent pe[2];
  size_t count = 0;
  const auto ts = FlutterTimestampMicros();

  if (!pointer_added_) {
    pe[count] = FlutterPointerEvent{};
    pe[count].struct_size = sizeof(FlutterPointerEvent);
    pe[count].phase = kAdd;
    pe[count].timestamp = ts;
    pe[count].x = pointer_x_;
    pe[count].y = pointer_y_;
    pe[count].device = 0;
    pe[count].device_kind = kFlutterPointerDeviceKindMouse;
    pe[count].buttons = 0;
    pointer_added_ = true;
    ++count;
  }

  pe[count] = FlutterPointerEvent{};
  pe[count].struct_size = sizeof(FlutterPointerEvent);
  pe[count].phase = button_mask_ ? kMove : kHover;
  pe[count].timestamp = ts;
  pe[count].x = pointer_x_;
  pe[count].y = pointer_y_;
  pe[count].device = 0;
  pe[count].device_kind = kFlutterPointerDeviceKindMouse;
  pe[count].buttons = button_mask_;
  ++count;

  LibFlutterEngine->SendPointerEvent(engine, pe, count);
}

void DrmSeat::HandlePointerButton(const drm::input::PointerButtonEvent& ev) {
  auto engine = CurrentEngine();
  if (!engine) {
    return;
  }

  const int64_t bit = MapMouseButton(ev.button);
  if (bit == 0) {
    return;
  }
  const int64_t prev_mask = button_mask_;
  if (ev.pressed) {
    button_mask_ |= bit;
  } else {
    button_mask_ &= ~bit;
  }

  FlutterPointerPhase phase;
  if (ev.pressed) {
    // A press while another button is already held becomes kMove; kDown is
    // reserved for the first-button-down transition.
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

  LibFlutterEngine->SendPointerEvent(engine, &pe, 1);
}

void DrmSeat::HandlePointerAxis(const drm::input::PointerAxisEvent& ev) {
  auto engine = CurrentEngine();
  if (!engine) {
    return;
  }

  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = button_mask_ ? kMove : kHover;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = pointer_x_;
  pe.y = pointer_y_;
  pe.device = 0;
  pe.device_kind = kFlutterPointerDeviceKindMouse;
  pe.buttons = button_mask_;
  pe.signal_kind = kFlutterPointerSignalKindScroll;
  pe.scroll_delta_x = ev.horizontal;
  pe.scroll_delta_y = ev.vertical;

  LibFlutterEngine->SendPointerEvent(engine, &pe, 1);
}

void DrmSeat::HandleTouch(const drm::input::TouchEvent& ev) {
  // Frame is an atomic-batch delimiter; nothing to forward.
  if (ev.type == drm::input::TouchEvent::Type::Frame) {
    return;
  }

  auto engine = CurrentEngine();
  if (!engine) {
    return;
  }

  FlutterPointerPhase phase = kMove;
  switch (ev.type) {
    case drm::input::TouchEvent::Type::Down:
      phase = kDown;
      break;
    case drm::input::TouchEvent::Type::Up:
      phase = kUp;
      break;
    case drm::input::TouchEvent::Type::Motion:
      phase = kMove;
      break;
    case drm::input::TouchEvent::Type::Cancel:
      phase = kCancel;
      break;
    case drm::input::TouchEvent::Type::Frame:
      return;
  }

  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = phase;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = ev.x;
  pe.y = ev.y;
  pe.device = ev.slot;
  pe.device_kind = kFlutterPointerDeviceKindTouch;
  pe.buttons = 0;

  LibFlutterEngine->SendPointerEvent(engine, &pe, 1);
}

}  // namespace homescreen
