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

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <csignal>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <variant>

#include "asio/dispatch.hpp"
#include "asio/post.hpp"
#include "engine.h"
#include "input/key_mapping.h"
#include "libflutter_engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "shell/platform/homescreen/flutter_desktop_view_controller_state.h"
#include "task_runner.h"

namespace homescreen {
namespace {
constexpr int kPollTimeoutMs = 100;

size_t FlutterTimestampMicros() {
  return static_cast<size_t>(LibFlutterEngine->GetCurrentTime() / 1000);
}

int64_t MapMouseButton(const uint32_t evdev_button) {
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

// ---- TTY keyboard-mode backstop ----
//
// When DrmSeat switches the VT to K_OFF, the kernel line discipline
// stops echoing keystrokes to the shell underneath. A normal shutdown
// restores it in Stop(), but if the process dies via SIGKILL, a fatal
// signal (SEGV/ABRT/…), or std::terminate, the VT stays in K_OFF and
// the console becomes deaf — the user can't even type `reboot`.
//
// Register an atexit handler for the normal-exit path plus a small set
// of async-signal-safe fatal-signal handlers that restore the mode and
// then re-raise with the default disposition so core dumps still work.
// Stop() hands the fd off to this backstop before we close it, so the
// backstop is a no-op once normal teardown has happened.
std::atomic<int> g_tty_fd{-1};
std::atomic<int> g_saved_kb_mode{0};
std::once_flag g_backstop_installed;

void RestoreTtyKeyboardMode() noexcept {
  // exchange so concurrent callers (fatal handler racing atexit/Stop)
  // cooperate — whoever takes the fd does the ioctl+close, everyone
  // else short-circuits.
  const int fd = g_tty_fd.exchange(-1, std::memory_order_acq_rel);
  if (fd < 0) {
    return;
  }
  const int mode = g_saved_kb_mode.load(std::memory_order_acquire);
  // Async-signal-safe: just ioctl + close. No logging.
  ioctl(fd, KDSKBMODE, mode);
  ::close(fd);
}

extern "C" void BackstopFatalHandler(int sig) {
  RestoreTtyKeyboardMode();
  // Handler was installed with SA_RESETHAND, so sig is now SIG_DFL.
  // Re-raise so the default action (terminate, possibly with core) is
  // taken once the handler returns.
  raise(sig);
}

void InstallBackstop() {
  std::atexit(&RestoreTtyKeyboardMode);
  struct sigaction sa{};
  sa.sa_handler = &BackstopFatalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  for (const int sig : {SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS}) {
    sigaction(sig, &sa, nullptr);
  }
}
}  // namespace

DrmSeat::DrmSeat(const int32_t viewport_width, const int32_t viewport_height)
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
  const auto* state = state_.load(std::memory_order_acquire);
  if (!state || !state->engine) {
    return nullptr;
  }
  return state->engine->GetFlutterEngine();
}

bool DrmSeat::Start() {
  if (seat_) {
    return true;
  }

  // Without K_OFF on a bare text VT, keystrokes are also consumed by the
  // kernel tty line discipline (echoed to the shell underneath). K_OFF
  // disables that — it does NOT affect libinput/xkbcommon translation,
  // which is done by keyboard_ below. Restored in Stop().
  tty_fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (tty_fd_ >= 0) {
    if (ioctl(tty_fd_, KDGKBMODE, &saved_kb_mode_) == 0) {
      // Arm the reverse-watchdog BEFORE mutating the kb mode. If the
      // parent is SIGKILL'd before Stop() runs, the child restores
      // saved_kb_mode_ so the console keyboard comes back.
      tty_watchdog_ =
          homescreen::watchdog::SpawnTtyRestore(tty_fd_, saved_kb_mode_);

      if (ioctl(tty_fd_, KDSKBMODE, K_OFF) == 0) {
        spdlog::info("[DrmSeat] VT keyboard mode set to K_OFF (was {})",
                     saved_kb_mode_);

        // Publish the fd + original mode for the crash/atexit backstop.
        // Store the mode first so any concurrent restore sees a valid
        // value once the fd is visible.
        g_saved_kb_mode.store(saved_kb_mode_, std::memory_order_release);
        g_tty_fd.store(tty_fd_, std::memory_order_release);
        std::call_once(g_backstop_installed, &InstallBackstop);
      } else {
        // Nothing mutated — drop the watchdog.
        homescreen::watchdog::Disarm(tty_watchdog_);
        spdlog::warn("[DrmSeat] KDSKBMODE K_OFF failed: {}",
                     std::strerror(errno));
      }
    }
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

  // drm-cxx leaves KeyboardEvent::sym/utf8 zero unless the caller runs
  // events through a Keyboard (xkbcommon state machine). Without this,
  // every key is dropped at the sym==0 check in HandleKeyboard.
  auto kb = drm::input::Keyboard::create();
  if (!kb) {
    spdlog::error("[DrmSeat] xkb keymap create failed: {}",
                  kb.error().message());
    return false;
  }
  keyboard_ = std::make_unique<drm::input::Keyboard>(std::move(*kb));

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

  // Restore VT keyboard mode so the text console works after exit.
  // Take the fd out of the backstop first so atexit/fatal handlers don't
  // race us on the same fd.
  if (tty_fd_ >= 0) {
    g_tty_fd.store(-1, std::memory_order_release);
    ioctl(tty_fd_, KDSKBMODE, saved_kb_mode_);
    spdlog::info("[DrmSeat] VT keyboard mode restored to {}", saved_kb_mode_);
    ::close(tty_fd_);
    tty_fd_ = -1;
  }
  // Reverse-watchdog no longer needed: we restored the mode ourselves.
  homescreen::watchdog::Disarm(tty_watchdog_);
}

void DrmSeat::DispatchLoop() const {
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

void DrmSeat::HandleKeyboard(const drm::input::KeyboardEvent& ev) const {
  drm::input::KeyboardEvent resolved = ev;
  if (keyboard_) {
    keyboard_->process_key(resolved);
  }

  const uint64_t physical = keys::EvdevToPhysical(resolved.key);
  const uint64_t logical = keys::DeriveLogicalKey(resolved.utf8, resolved.sym);

  if (physical == 0 || logical == 0) {
    spdlog::warn(
        "[DrmSeat] dropping key={} sym=0x{:x} utf8='{}' (phys=0x{:x} "
        "log=0x{:x})",
        resolved.key, resolved.sym, resolved.utf8, physical, logical);
    return;
  }

  auto* s = state_.load(std::memory_order_acquire);
  if (!s || !s->engine) {
    return;
  }

  FlutterKeyEvent ke{};
  ke.struct_size = sizeof(FlutterKeyEvent);
  ke.timestamp = static_cast<double>(FlutterTimestampMicros());
  ke.type =
      resolved.pressed ? kFlutterKeyEventTypeDown : kFlutterKeyEventTypeUp;
  ke.physical = physical;
  ke.logical = logical;
  ke.synthesized = false;
  ke.device_type = kFlutterKeyEventDeviceTypeKeyboard;

  // Flutter's hardware_keyboard.dart asserts that `character` is null
  // for KEY_UP events (and synthesized events). Attach it only to
  // DOWN transitions — repeat/up carry physical+logical only.
  std::string character_owned;
  if (resolved.pressed && resolved.utf8[0] != '\0') {
    character_owned = resolved.utf8;
  }

  auto* runner = s->engine->GetPlatformTaskRunner();
  auto engine_handle = s->engine->GetFlutterEngine();
  if (runner && engine_handle) {
    asio::post(*runner->GetStrandContext(),
               [engine_handle, ke, ch = std::move(character_owned)]() {
                 FlutterKeyEvent event = ke;
                 event.character = ch.empty() ? nullptr : ch.c_str();
                 LibFlutterEngine->SendKeyEvent(engine_handle, &event, nullptr,
                                                nullptr);
               });
  }
}

void DrmSeat::HandlePointerMotion(const drm::input::PointerMotionEvent& ev) {
  if (const auto engine = CurrentEngine(); !engine) {
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

  const auto* s = state_.load(std::memory_order_acquire);
  if (!s || !s->engine) {
    return;
  }
  const auto* runner = s->engine->GetPlatformTaskRunner();
  if (auto engine_handle = s->engine->GetFlutterEngine();
      runner && engine_handle) {
    std::vector events(pe, pe + count);
    asio::dispatch(*runner->GetStrandContext(),
                   [engine_handle, events = std::move(events)]() {
                     LibFlutterEngine->SendPointerEvent(
                         engine_handle, events.data(), events.size());
                   });
  }
}

void DrmSeat::HandlePointerButton(const drm::input::PointerButtonEvent& ev) {
  // CurrentEngine() is only for the null-check; actual dispatch goes via
  // state_.
  if (!CurrentEngine()) {
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

  auto* s = state_.load(std::memory_order_acquire);
  if (!s || !s->engine) {
    return;
  }
  const auto* runner = s->engine->GetPlatformTaskRunner();
  if (auto engine_handle = s->engine->GetFlutterEngine();
      runner && engine_handle) {
    asio::dispatch(*runner->GetStrandContext(), [engine_handle, pe]() {
      LibFlutterEngine->SendPointerEvent(engine_handle, &pe, 1);
    });
  }
}

void DrmSeat::HandlePointerAxis(const drm::input::PointerAxisEvent& ev) const {
  if (!CurrentEngine()) {
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

  auto* s = state_.load(std::memory_order_acquire);
  if (!s || !s->engine) {
    return;
  }
  const auto* runner = s->engine->GetPlatformTaskRunner();
  if (auto engine_handle = s->engine->GetFlutterEngine();
      runner && engine_handle) {
    asio::dispatch(*runner->GetStrandContext(), [engine_handle, pe]() {
      LibFlutterEngine->SendPointerEvent(engine_handle, &pe, 1);
    });
  }
}

void DrmSeat::HandleTouch(const drm::input::TouchEvent& ev) const {
  if (ev.type == drm::input::TouchEvent::Type::Frame) {
    return;
  }
  if (!CurrentEngine()) {
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

  const auto* s = state_.load(std::memory_order_acquire);
  if (!s || !s->engine) {
    return;
  }
  const auto* runner = s->engine->GetPlatformTaskRunner();
  if (auto engine_handle = s->engine->GetFlutterEngine();
      runner && engine_handle) {
    asio::dispatch(*runner->GetStrandContext(), [engine_handle, pe]() {
      LibFlutterEngine->SendPointerEvent(engine_handle, &pe, 1);
    });
  }
}
}  // namespace homescreen
