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

#include "config/common.h"
#include "logging/logging.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <csignal>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "asio/dispatch.hpp"
#include "asio/post.hpp"
#include "backend/drm_kms_egl/drm_cursor.h"
#include "engine.h"
#include "libflutter_engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "shell/platform/homescreen/flutter_desktop_view_controller_state.h"
#include "task_runner.h"

// Defined in flutter_desktop.cc. Fans a key event out to the platform
// keyboard-hook handlers (flutter/keyevent channel + TextInputPlugin) — the
// same entry point the Wayland and software seats dispatch through.
extern void KeyCallback(FlutterDesktopViewControllerState* view_state,
                        bool released,
                        xkb_keysym_t keysym,
                        uint32_t xkb_scancode,
                        uint32_t modifiers);

namespace homescreen {
namespace {
// Degraded-mode poll cap, used only when the waker eventfd is unavailable
// (eventfd() failed at construction). In the normal path the dispatch loop
// blocks in poll(-1) and is woken explicitly via waker_.
constexpr int kPollTimeoutMs = 100;

// Strip control bytes from user-supplied strings before logging. The
// RMLVO fields fed to ReloadKeymap come from a future settings channel
// (Flutter platform-message → here), so the values must be treated as
// untrusted: bytes < 0x20 or 0x7f would otherwise flow into stdout
// sinks as terminal control sequences. Mirrors the helper in
// driver_probe.cc.
std::string Sanitize(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    out.push_back((u < 0x20 || u == 0x7f) ? '?' : c);
  }
  return out;
}

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
// backstop is a no-op once a normal teardown has happened.
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
  sa.sa_flags = static_cast<int>(SA_RESETHAND);
  for (const int sig : {SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS}) {
    sigaction(sig, &sa, nullptr);
  }
}
}  // namespace

DrmSeat::DrmSeat(const int32_t viewport_width,
                 const int32_t viewport_height,
                 drm::input::InputDeviceOpener opener)
    : opener_(std::move(opener)) {
  // Seed the region-under-construction with the initial viewport. The first
  // SetViewControllerState commits it as the primary region and centers the
  // pointer on it.
  pending_.width = viewport_width;
  pending_.height = viewport_height;
}

DrmSeat::~DrmSeat() {
  Stop();
}

void DrmSeat::SetViewControllerState(FlutterDesktopViewControllerState* state) {
  // A null state is the teardown clear; DRM views always register a non-null
  // controller, so there is nothing to commit for null.
  if (state == nullptr) {
    return;
  }
  // Commit the region accumulated by the per-view setters (SetViewport,
  // SetCursor, SetGlCursor, SetCursorRotation, SetRegionLayout) into the
  // region set, then reset the builder for the next view. Registration runs on
  // the main thread during view construction, before Start() spawns the
  // dispatch thread, so no lock is needed.
  pending_.state = state;
  // If this region's origin collides with an existing one -- e.g. two bundles
  // launched without explicit [view.output] x/y, so both defaulted to (0,0) --
  // tile it to the right of the current set. Otherwise the regions would stack
  // and only the first would ever be under the pointer. Explicit,
  // non-overlapping layouts never collide, so they pass through untouched.
  for (const auto& r : regions_) {
    if (r.layout_x == pending_.layout_x && r.layout_y == pending_.layout_y) {
      int32_t right = 0;
      for (const auto& e : regions_) {
        right = std::max(right, e.layout_x + e.width);
      }
      pending_.layout_x = right;
      pending_.layout_y = 0;
      break;
    }
  }
  const bool first = regions_.empty();
  regions_.push_back(pending_);
  if (first) {
    // Center the global pointer on the primary region so the first hover is
    // visible before the user moves the mouse.
    const auto& r = regions_.front();
    pointer_x_ = r.layout_x + r.width / 2.0;
    pointer_y_ = r.layout_y + r.height / 2.0;
    active_region_ = 0;
  }
  pending_ = ViewRegion{};
}

FLUTTER_API_SYMBOL(FlutterEngine) DrmSeat::CurrentEngine() const {
  if (regions_.empty() || active_region_ >= regions_.size()) {
    return nullptr;
  }
  const auto* state = regions_[active_region_].state;
  if (!state || !state->engine) {
    return nullptr;
  }
  return state->engine->GetFlutterEngine();
}

void DrmSeat::SetRegionLayout(const int32_t x, const int32_t y) {
  pending_.layout_x = x;
  pending_.layout_y = y;
}

void DrmSeat::SetRegionTouchDevice(std::string name) {
  pending_.touch_match = std::move(name);
}

void DrmSeat::ResolveActiveRegion() {
  if (regions_.empty()) {
    return;
  }
  const auto contains = [](const ViewRegion& r, double x, double y) {
    return x >= r.layout_x && x < r.layout_x + r.width && y >= r.layout_y &&
           y < r.layout_y + r.height;
  };
  // Fast path: still inside the current region.
  if (active_region_ < regions_.size() &&
      contains(regions_[active_region_], pointer_x_, pointer_y_)) {
    return;
  }
  // Crossed into another region?
  for (size_t i = 0; i < regions_.size(); ++i) {
    if (contains(regions_[i], pointer_x_, pointer_y_)) {
      active_region_ = i;
      return;
    }
  }
  // In a gap or past the outer edge: clamp to the union bounding box, then
  // re-test so an overshoot at a shared border still lands in the neighbor.
  int32_t min_x = regions_[0].layout_x;
  int32_t min_y = regions_[0].layout_y;
  int32_t max_x = regions_[0].layout_x + regions_[0].width;
  int32_t max_y = regions_[0].layout_y + regions_[0].height;
  for (const auto& r : regions_) {
    min_x = std::min(min_x, r.layout_x);
    min_y = std::min(min_y, r.layout_y);
    max_x = std::max(max_x, r.layout_x + r.width);
    max_y = std::max(max_y, r.layout_y + r.height);
  }
  pointer_x_ = std::clamp(pointer_x_, static_cast<double>(min_x),
                          static_cast<double>(max_x - 1));
  pointer_y_ = std::clamp(pointer_y_, static_cast<double>(min_y),
                          static_cast<double>(max_y - 1));
  for (size_t i = 0; i < regions_.size(); ++i) {
    if (contains(regions_[i], pointer_x_, pointer_y_)) {
      active_region_ = i;
      return;
    }
  }
  // Still in a dead zone (non-contiguous layout with mismatched extents): keep
  // the pointer inside the current region so it can't get stuck between
  // displays.
  const size_t keep = active_region_ < regions_.size() ? active_region_ : 0;
  const auto& r = regions_[keep];
  pointer_x_ = std::clamp(pointer_x_, static_cast<double>(r.layout_x),
                          static_cast<double>(r.layout_x + r.width - 1));
  pointer_y_ = std::clamp(pointer_y_, static_cast<double>(r.layout_y),
                          static_cast<double>(r.layout_y + r.height - 1));
  active_region_ = keep;
}

bool DrmSeat::RegionsShareEngine(const size_t a, const size_t b) const {
  if (a >= regions_.size() || b >= regions_.size()) {
    return false;
  }
  const auto* sa = regions_[a].state;
  const auto* sb = regions_[b].state;
  return sa != nullptr && sb != nullptr && sa->engine == sb->engine;
}

void DrmSeat::DispatchToRegion(const ViewRegion& region,
                               const FlutterPointerEvent* events,
                               const size_t count) {
  const auto* s = region.state;
  if (s == nullptr || s->engine == nullptr) {
    return;
  }
  const auto* runner = s->engine->GetPlatformTaskRunner();
  auto engine_handle = s->engine->GetFlutterEngine();
  if (runner == nullptr || engine_handle == nullptr) {
    return;
  }
  std::vector<FlutterPointerEvent> batch(events, events + count);
  asio::dispatch(*runner->GetStrandContext(),
                 [engine_handle, batch = std::move(batch)]() {
                   LibFlutterEngine->SendPointerEvent(
                       engine_handle, batch.data(), batch.size());
                 });
}

bool DrmSeat::Start() {
  if (seat_) {
    return true;
  }

  const bool have_session = !opener_.empty();

  // Without K_OFF on a bare text VT, keystrokes are also consumed by the
  // kernel tty line discipline (echoed to the shell underneath). K_OFF
  // disables that — it does NOT affect libinput/xkbcommon translation,
  // which is done by keyboard_ below. Restored in Stop().
  //
  // When libseat is driving the session, logind/seatd manages VT
  // keyboard state as part of session activation — we must not fight
  // it. Skip the K_OFF path entirely in that mode.
  if (!have_session) {
    tty_fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty_fd_ >= 0) {
      if (ioctl(tty_fd_, KDGKBMODE, &saved_kb_mode_) == 0) {
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
          spdlog::warn("[DrmSeat] KDSKBMODE K_OFF failed: {}",
                       std::strerror(errno));
        }
      }
    }
  }

  auto opened = have_session ? drm::input::Seat::open({}, std::move(opener_))
                             : drm::input::Seat::open();
  if (!opened) {
    spdlog::error("[DrmSeat] libinput seat open failed: {}",
                  opened.error().message());
    return false;
  }
  seat_ = std::make_unique<drm::input::Seat>(std::move(*opened));
  seat_->set_event_handler(
      [this](const drm::input::InputEvent& ev) { HandleEvent(ev); });

  // Keyboard translation comes from the backend-shared XkbKeyboard (xkbcommon
  // state machine), the same path SoftwareSeat uses. drm::input::Seat leaves
  // KeyboardEvent::sym/utf8 zero; HandleKeyboard fills them in, otherwise every
  // key is dropped at the sym==0 check in DispatchKeyToFlutter.
  keyboard_ = input::XkbKeyboard::Create();
  if (!keyboard_) {
    spdlog::error("[DrmSeat] XkbKeyboard::Create failed");
    return false;
  }

  // Auto-repeat for held keys (libinput doesn't repeat by design — the embedder
  // synthesizes it). The shared KeyRepeater is timerfd-driven and re-resolves
  // sym/utf8 every tick so modifier changes mid-hold (Shift, AltGr) apply to
  // the next repeat. Failure is non-fatal. IVI_DRM_KEY_REPEAT=0 disables.
  const char* repeat_gate = std::getenv("IVI_DRM_KEY_REPEAT");
  if (repeat_gate == nullptr || std::string_view(repeat_gate) != "0") {
    repeater_ = input::KeyRepeater::Create(keyboard_.get());
    if (repeater_) {
      repeater_->SetHandler(
          [this](uint32_t key, xkb_keysym_t sym, const std::string& utf8) {
            // Re-pack the synthesized repeat into the drm-cxx KeyboardEvent
            // carrier that DispatchKeyToFlutter consumes.
            drm::input::KeyboardEvent e{};
            e.key = key;
            e.pressed = true;
            e.repeat = true;
            e.sym = sym;
            std::snprintf(e.utf8, sizeof(e.utf8), "%s", utf8.c_str());
            DispatchKeyToFlutter(e);
          });
    } else {
      spdlog::warn("[DrmSeat] KeyRepeater unavailable (timerfd_create)");
    }
  }

  // Sync the physical LEDs to the keyboard's initial (all-off) latch so a
  // console that left Caps/Num lit doesn't stay stuck on after we take over.
  SyncKeyboardLeds();

  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this] { DispatchLoop(); });
  spdlog::info("[DrmSeat] started");
  return true;
}

void DrmSeat::Stop() {
  stop_.store(true, std::memory_order_release);
  // Store-then-wake: the dispatch thread is blocked in poll(-1); Wake() makes
  // the waker fd readable so the poll returns and the loop re-checks stop_.
  waker_.Wake();
  if (thread_.joinable()) {
    thread_.join();
  }
  // Defensive: cancel any in-flight repeat before the repeater destructs.
  if (repeater_) {
    repeater_->Cancel();
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
}

void DrmSeat::DispatchLoop() {
  pthread_setname_np(pthread_self(), "li-drm-dispatch");

  const int seat_fd = seat_->fd();
  int repeat_fd = repeater_ ? repeater_->fd() : -1;
  const int wake_fd = waker_.fd();
  constexpr short kErr = POLLERR | POLLHUP | POLLNVAL;
  // No timeout in the normal path: the loop blocks until the seat fd, the
  // repeat timerfd, or the waker is ready. Each cross-thread flag (stop_,
  // session pause/resume, keymap reload) is paired with a waker_.Wake(), so the
  // poll returns and ApplyPendingKeymap() + the session-action exchange at the
  // loop top run. Degraded mode (no waker fd) falls back to the 100 ms cap.
  const int timeout_ms = wake_fd >= 0 ? -1 : kPollTimeoutMs;

  while (!stop_.load(std::memory_order_acquire)) {
    ApplyPendingKeymap();

    // Drain any pending session pause/resume action posted by the
    // libseat dispatch thread. libinput suspend/resume MUST run on the
    // thread that owns the libinput context (this one). On suspend,
    // libinput closes its evdev fds via close_restricted, which
    // releases them through libseat. On resume, libinput reopens via
    // open_restricted, which routes back through libseat and gets
    // fresh post-VT-switch fds. Without this dance, post-resume
    // libinput is reading from revoked fds and produces no events —
    // the symptom is the chord working once and then never again.
    switch (const auto act = pending_session_action_.exchange(
                PendingSessionAction::kNone, std::memory_order_acq_rel);
            act) {
      case PendingSessionAction::kSuspend:
        if (seat_) {
          if (auto r = seat_->suspend(); !r) {
            spdlog::warn("[DrmSeat] libinput suspend: {}", r.error().message());
          } else {
            spdlog::info("[DrmSeat] libinput suspended for VT switch-out");
          }
        }
        // Cancel any in-flight key repeat: a key held at switch-out would
        // otherwise keep the repeat timerfd ticking into a backgrounded view.
        if (repeater_) {
          repeater_->Cancel();
        }
        break;
      case PendingSessionAction::kResume:
        if (seat_) {
          if (auto r = seat_->resume(); !r) {
            spdlog::warn("[DrmSeat] libinput resume: {}", r.error().message());
          } else {
            spdlog::info("[DrmSeat] libinput resumed after VT switch-in");
          }
        }
        break;
      case PendingSessionAction::kNone:
        break;
    }

    pollfd pfds[3];
    pfds[0] = {seat_fd, POLLIN, 0};
    nfds_t nfds = 1;
    int repeat_idx = -1;
    if (repeat_fd >= 0) {
      repeat_idx = static_cast<int>(nfds);
      pfds[nfds++] = {repeat_fd, POLLIN, 0};
    }
    int wake_idx = -1;
    if (wake_fd >= 0) {
      wake_idx = static_cast<int>(nfds);
      pfds[nfds++] = {wake_fd, POLLIN, 0};
    }
    const int r = poll(pfds, nfds, timeout_ms);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[DrmSeat] poll: {}", std::strerror(errno));
      break;
    }
    if (r > 0) {
      // The waker is a pure "re-check your flags" nudge: drain and loop so the
      // stop_ test + ApplyPendingKeymap() + the session-action exchange at the
      // top run. An error on the waker fd can't happen short of fd corruption;
      // treat it as fatal.
      if (wake_idx >= 0) {
        if ((pfds[wake_idx].revents & kErr) != 0) {
          spdlog::error("[DrmSeat] waker fd error (revents={:#x}); exiting",
                        static_cast<unsigned>(pfds[wake_idx].revents));
          break;
        }
        if ((pfds[wake_idx].revents & POLLIN) != 0) {
          waker_.Drain();
          continue;
        }
      }
      if ((pfds[0].revents & kErr) != 0) {
        spdlog::error("[DrmSeat] seat fd error (revents={:#x}); exiting",
                      static_cast<unsigned>(pfds[0].revents));
        break;
      }
      if ((pfds[0].revents & POLLIN) != 0) {
        if (auto ok = seat_->dispatch(); !ok) {
          spdlog::warn("[DrmSeat] dispatch: {}", ok.error().message());
        }
        FlushCursorMotion();
      }
      if (repeat_idx >= 0) {
        if ((pfds[repeat_idx].revents & kErr) != 0) {
          // Repeater timerfd died (HUP/ERR/NVAL). Drop it and keep
          // serving the seat — otherwise poll() returns POLLERR forever
          // and the loop spins.
          spdlog::warn(
              "[DrmSeat] repeater fd error (revents={:#x}); disabling repeat",
              static_cast<unsigned>(pfds[repeat_idx].revents));
          repeater_.reset();
          repeat_fd = -1;
        } else if ((pfds[repeat_idx].revents & POLLIN) != 0) {
          repeater_->Dispatch();
        }
      }
    }
  }
}

void DrmSeat::FlushCursorMotion() {
  if (!cursor_motion_pending_ || regions_.empty() ||
      active_region_ >= regions_.size()) {
    return;
  }
  cursor_motion_pending_ = false;

  // The sprite lives on the display the pointer is over; drive it in that
  // region's local (render) coordinates.
  const ViewRegion& region = regions_[active_region_];
  const int rx = static_cast<int>(pointer_x_ - region.layout_x);
  const int ry = static_cast<int>(pointer_y_ - region.layout_y);

  // The GL-composited cursor (fallback for no HW cursor plane) draws into the
  // render target, so it takes un-rotated render/viewport coordinates — the
  // display applies any scanout rotation to the whole framebuffer downstream.
  if (auto* g = region.gl_cursor; g != nullptr) {
    g->SetPosition(rx, ry);
    // The composited cursor only repaints on a presented frame, and Flutter
    // presents only dirty frames — so on an idle UI the cursor would freeze
    // between the vsync-paced frames the app happens to produce. Nudge the
    // engine to render so the cursor tracks the pointer. ScheduleFrame is
    // thread-safe and coalesces to one frame per vsync. (A HW cursor plane,
    // when present, self-commits and needs no frame — hence composited-only.)
    if (const auto engine = CurrentEngine(); engine != nullptr) {
      LibFlutterEngine->ScheduleFrame(engine);
    }
  }

#if HAVE_DRM_CURSOR
  // The pointer changed displays: hide the sprite on the one it left and show
  // the one it entered, so exactly one cursor is ever visible. Without this a
  // fast cross leaves the departed plane committed at its last (mid-screen)
  // position -- a second, frozen cursor. (The composited gl_cursor fallback
  // path has the same issue on HW without a cursor plane; hiding it needs a
  // repaint and is a separate pass.)
  if (cursor_shown_region_ != active_region_) {
    if (cursor_shown_region_ != kNoRegion &&
        cursor_shown_region_ < regions_.size()) {
      if (auto* prev = regions_[cursor_shown_region_].cursor; prev != nullptr) {
        prev->Hide();
      }
    }
    if (region.cursor != nullptr) {
      region.cursor->Show();  // no-op if it was never hidden
    }
    cursor_shown_region_ = active_region_;
  }
  if (auto* c = region.cursor; c != nullptr) {
    // The HW cursor plane lives in panel space. For a 90/270 scanout rotation
    // that differs from render space (axes swapped), so rotate the position by
    // the scanout amount before placing the sprite. 0/180 leave the extent
    // unchanged. (Flutter pointer events keep the un-rotated render coords, so
    // hit-testing stays correct.)
    int px = rx;
    int py = ry;
    switch (region.cursor_rotation) {
      case 90:
        px = ry;
        py = region.width - 1 - rx;
        break;
      case 180:
        px = region.width - 1 - rx;
        py = region.height - 1 - ry;
        break;
      case 270:
        px = region.height - 1 - ry;
        py = rx;
        break;
      default:
        break;
    }
    // SetPosition records the position for the compositor to stage (staged
    // mode) or self-commits via Move (legacy). Either way, no DRM commit
    // here in staged mode — the compositor owns the cursor plane.
    c->SetPosition(px, py);
  }
#endif
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

void DrmSeat::ReloadKeymap(KeymapConfig cfg) {
  {
    std::lock_guard lk(pending_mu_);
    pending_keymap_ = std::move(cfg);
  }
  // Wake the dispatch thread so ApplyPendingKeymap() runs now rather than on
  // the next input event.
  waker_.Wake();
}

void DrmSeat::SetVtSwitchHandler(VtSwitchHandler handler) {
  std::lock_guard lk(vt_switch_mu_);
  vt_switch_handler_ = std::move(handler);
}

void DrmSeat::SetViewport(const int32_t width, const int32_t height) {
  // Sizes the region-under-construction; the pointer is centered on the
  // primary region when it commits (see SetViewControllerState).
  pending_.width = width;
  pending_.height = height;
}

void DrmSeat::SetInputTransforms(const std::vector<std::string>& specs) {
  pointer_transforms_.clear();
  for (const auto& spec : specs) {
    const auto eq = spec.rfind('=');
    if (eq == std::string::npos || eq == 0) {
      spdlog::warn(
          "[DrmSeat] --input-transform '{}' ignored "
          "(expected <name>=<0|90|180|270>[,flip-x][,flip-y])",
          Sanitize(spec));
      continue;
    }
    PointerTransform t;
    t.match = spec.substr(0, eq);
    bool valid = true;
    const std::string rest = spec.substr(eq + 1);
    for (size_t start = 0; start <= rest.size();) {
      const auto comma = rest.find(',', start);
      const std::string tok =
          rest.substr(start, comma == std::string::npos ? std::string::npos
                                                        : comma - start);
      if (tok == "flip-x") {
        t.flip_x = true;
      } else if (tok == "flip-y") {
        t.flip_y = true;
      } else if (tok == "0") {
        t.rotation = 0;
      } else if (tok == "90") {
        t.rotation = 90;
      } else if (tok == "180") {
        t.rotation = 180;
      } else if (tok == "270") {
        t.rotation = 270;
      } else {
        spdlog::warn("[DrmSeat] --input-transform '{}': bad token '{}'",
                     Sanitize(spec), Sanitize(tok));
        valid = false;
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    if (valid) {
      spdlog::info(
          "[DrmSeat] input-transform: match='{}' rot={} flip_x={} flip_y={}",
          Sanitize(t.match), t.rotation, t.flip_x, t.flip_y);
      pointer_transforms_.push_back(std::move(t));
    }
  }
}

void DrmSeat::OnSessionPaused() {
  pending_session_action_.store(PendingSessionAction::kSuspend,
                                std::memory_order_release);
  // Posted from the libseat dispatch thread. Wake this seat's dispatch thread
  // so it services the suspend now; under poll(-1) it would otherwise stall
  // until the next input event — but the fds are about to be revoked, so no
  // event is coming (the revoked-fd hang the suspend dance exists to prevent).
  waker_.Wake();
}

void DrmSeat::OnSessionResumed() {
  pending_session_action_.store(PendingSessionAction::kResume,
                                std::memory_order_release);
  // Same rationale as OnSessionPaused: the resume must run on the dispatch
  // thread, and no input can arrive until libinput is resumed, so the waker is
  // the only thing that can unblock the poll.
  waker_.Wake();
}

void DrmSeat::ApplyPendingKeymap() {
  std::optional<KeymapConfig> cfg;
  {
    std::lock_guard lk(pending_mu_);
    cfg.swap(pending_keymap_);
  }
  if (!cfg || !keyboard_) {
    return;
  }
  input::KeymapOptions opts;
  opts.rules = cfg->rules;
  opts.model = cfg->model;
  opts.layout = cfg->layout;
  opts.variant = cfg->variant;
  opts.options = cfg->options;
  if (!keyboard_->Reload(opts)) {
    spdlog::warn(
        "[DrmSeat] keymap reload failed (rules='{}' model='{}' layout='{}' "
        "variant='{}' options='{}') — existing keymap kept",
        Sanitize(cfg->rules), Sanitize(cfg->model), Sanitize(cfg->layout),
        Sanitize(cfg->variant), Sanitize(cfg->options));
    return;
  }
  spdlog::info(
      "[DrmSeat] keymap reloaded (rules='{}' model='{}' layout='{}' "
      "variant='{}' options='{}')",
      Sanitize(cfg->rules), Sanitize(cfg->model), Sanitize(cfg->layout),
      Sanitize(cfg->variant), Sanitize(cfg->options));
  // Push the latched Caps/Num/Scroll Lock state to the physical LEDs so they
  // don't lag the freshly-rebuilt xkb state.
  SyncKeyboardLeds();
}

void DrmSeat::SyncKeyboardLeds() {
  if (seat_ == nullptr || keyboard_ == nullptr) {
    return;
  }
  const auto leds = keyboard_->Leds();
  seat_->update_keyboard_leds(drm::input::KeyboardLeds{
      leds.caps_lock, leds.num_lock, leds.scroll_lock});
}

void DrmSeat::HandleKeyboard(const drm::input::KeyboardEvent& ev) {
  drm::input::KeyboardEvent resolved = ev;
  if (keyboard_) {
    // Fill sym/utf8 and advance modifier state (the chord check below and the
    // repeater's per-key eligibility both rely on the updated state).
    std::string utf8;
    resolved.sym = keyboard_->ProcessKey(ev.key, ev.pressed, &utf8);
    std::snprintf(resolved.utf8, sizeof(resolved.utf8), "%s", utf8.c_str());
  }

  // VT-switch chord interception. With K_OFF the kernel keymap layer
  // doesn't dispatch Ctrl+Alt+F<n> for us; intercept the chord here and
  // route it through the installed handler (typically libseat
  // switch_session). Only on the press edge of the F-key — releases and
  // repeats are dropped so the chord doesn't fire repeatedly while held
  // and doesn't bounce Flutter on the release. Modifier-key events
  // (Ctrl/Alt themselves) skip this entirely so they still latch the
  // keyboard_ state above.
  if (resolved.pressed && !resolved.repeat && keyboard_ &&
      keyboard_->CtrlActive() && keyboard_->AltActive()) {
    int vt = 0;
    switch (resolved.key) {
      case KEY_F1:
        vt = 1;
        break;
      case KEY_F2:
        vt = 2;
        break;
      case KEY_F3:
        vt = 3;
        break;
      case KEY_F4:
        vt = 4;
        break;
      case KEY_F5:
        vt = 5;
        break;
      case KEY_F6:
        vt = 6;
        break;
      case KEY_F7:
        vt = 7;
        break;
      case KEY_F8:
        vt = 8;
        break;
      case KEY_F9:
        vt = 9;
        break;
      case KEY_F10:
        vt = 10;
        break;
      case KEY_F11:
        vt = 11;
        break;
      case KEY_F12:
        vt = 12;
        break;
      default:
        break;
    }
    if (vt != 0) {
      VtSwitchHandler handler;
      {
        std::lock_guard lk(vt_switch_mu_);
        handler = vt_switch_handler_;
      }
      if (handler && handler(vt)) {
        spdlog::info("[DrmSeat] Ctrl+Alt+F{} → VT switch requested", vt);
        // Consume the event: don't repeat-track, don't forward to Flutter.
        return;
      }
    }
  }

  // Track press/release in the repeater so it arms on press of an eligible key
  // and disarms on release. Only real events are fed here; synthesized repeats
  // come from the repeater's own handler, never back through HandleKeyboard.
  if (repeater_) {
    repeater_->OnKey(ev.key, ev.pressed);
  }
  DispatchKeyToFlutter(resolved);

  // A lock key just toggled the xkb latch — refresh the physical LEDs (the Seat
  // doesn't track the latch itself).
  if (resolved.sym == XKB_KEY_Caps_Lock || resolved.sym == XKB_KEY_Num_Lock ||
      resolved.sym == XKB_KEY_Scroll_Lock) {
    SyncKeyboardLeds();
  }
}

void DrmSeat::DispatchKeyToFlutter(
    const drm::input::KeyboardEvent& resolved) const {
  // Focus-follows-mouse: the keyboard goes to whichever view the pointer is
  // over. A single-view seat has one region, so this is the only view.
  if (regions_.empty() || active_region_ >= regions_.size()) {
    return;
  }
  auto* s = regions_[active_region_].state;
  if (s == nullptr) {
    return;
  }
  // Route through KeyCallback → the platform keyboard-hook handlers
  // (KeyEventHandler's flutter/keyevent channel + TextInputPlugin), exactly as
  // the Wayland and software backends do. That is the path the framework
  // actually consumes for text editing and Tab/arrow/lock keys. The DRM backend
  // previously used only the embedder SendKeyEvent (HardwareKeyboard) path,
  // which this app doesn't observe — so non-printable keys were silently
  // dropped while printable ones happened to round-trip. Going through
  // KeyCallback makes keyboard behavior identical across all three backends.
  const uint32_t modifiers = keyboard_ ? keyboard_->Modifiers() : 0;
  KeyCallback(s, !resolved.pressed, resolved.sym, resolved.key + 8, modifiers);
}

void DrmSeat::HandlePointerMotion(const drm::input::PointerMotionEvent& ev) {
  if (regions_.empty()) {
    return;  // no view has committed a region yet
  }

  // Apply a per-device delta transform when the source device matches a
  // configured rule (e.g. the Steam Deck's right trackpad, whose deltas are in
  // the rotated chassis frame). Unmatched devices (an external mouse) pass
  // through. First match wins.
  double dx = ev.dx;
  double dy = ev.dy;
  if (ev.device_name != nullptr && !pointer_transforms_.empty()) {
    const std::string_view name(ev.device_name);
    for (const auto& t : pointer_transforms_) {
      if (name.find(t.match) == std::string_view::npos) {
        continue;
      }
      double rx = dx;
      double ry = dy;
      switch (t.rotation) {
        case 90:
          rx = -dy;
          ry = dx;
          break;
        case 180:
          rx = -dx;
          ry = -dy;
          break;
        case 270:
          rx = dy;
          ry = -dx;
          break;
        default:
          break;
      }
      dx = t.flip_x ? -rx : rx;
      dy = t.flip_y ? -ry : ry;
      break;
    }
  }

  // Accumulate in combined (global) pointer space. The cursor moves freely
  // across displays -- even during a drag -- so it always tracks the hand;
  // ResolveActiveRegion clamps it to the display set and picks the region it
  // is physically over (which drives the sprite).
  pointer_x_ += dx;
  pointer_y_ += dy;

  const size_t prev = active_region_;
  ResolveActiveRegion();

  // Boundary decision during a drag: is the display the pointer is crossing
  // into part of the SAME engine (single engine spanning monitors -> let the
  // gesture cross freely) or a DIFFERENT engine (multi-engine -> isolate the
  // gesture to the origin engine by confining the cursor to its region)?
  if (grab_active_ && grab_region_ < regions_.size() &&
      !RegionsShareEngine(active_region_, grab_region_)) {
    const ViewRegion& g = regions_[grab_region_];
    pointer_x_ = std::clamp(pointer_x_, static_cast<double>(g.layout_x),
                            static_cast<double>(g.layout_x + g.width - 1));
    pointer_y_ = std::clamp(pointer_y_, static_cast<double>(g.layout_y),
                            static_cast<double>(g.layout_y + g.height - 1));
    active_region_ = grab_region_;
    // Known rough edge: the pointer stays pinned at the boundary during the
    // isolated drag, so if the mouse still carries an acceleration vector when
    // the button releases, the first free motion overshoots into the neighbor
    // display (a "bounce"). Damping that residual velocity on release is left
    // for a later refinement.
  }

  // Defer the cursor commit to FlushCursorMotion (called after the
  // dispatch batch). A blocking atomic cursor flip per libinput event
  // caps the seat thread at one vblank per event — at 60Hz that's 60
  // events/s, well under what a 125–1000Hz mouse delivers.
  cursor_motion_pending_ = true;

  const auto ts = FlutterTimestampMicros();

  // Hover focus only changes when NOT dragging. During an implicit grab the
  // pointer belongs to the grabbed view even while the cursor is over another
  // display, so we don't hand focus off. Otherwise the view the pointer left
  // needs a kRemove (its hover clears) and the one it entered a fresh kAdd.
  if (!grab_active_ && active_region_ != prev && prev < regions_.size() &&
      regions_[prev].pointer_added) {
    ViewRegion& old = regions_[prev];
    FlutterPointerEvent rm{};
    rm.struct_size = sizeof(FlutterPointerEvent);
    rm.phase = kRemove;
    rm.timestamp = ts;
    rm.x = pointer_x_ - old.layout_x;
    rm.y = pointer_y_ - old.layout_y;
    rm.device = 0;
    rm.device_kind = kFlutterPointerDeviceKindMouse;
    rm.buttons = 0;
    DispatchToRegion(old, &rm, 1);
    old.pointer_added = false;
  }

  // Events route to the grabbed view during a drag, else to the view under the
  // pointer. Coordinates are relative to the target view and may fall outside
  // its bounds mid-drag -- that is correct for a drag that left the window.
  const size_t target_idx = (grab_active_ && grab_region_ < regions_.size())
                                ? grab_region_
                                : active_region_;
  ViewRegion& target = regions_[target_idx];
  const double lx = pointer_x_ - target.layout_x;
  const double ly = pointer_y_ - target.layout_y;

  FlutterPointerEvent pe[2];
  size_t count = 0;

  if (!grab_active_ && !target.pointer_added) {
    // This view's engine must see the pointer device before any move/hover.
    pe[count] = FlutterPointerEvent{};
    pe[count].struct_size = sizeof(FlutterPointerEvent);
    pe[count].phase = kAdd;
    pe[count].timestamp = ts;
    pe[count].x = lx;
    pe[count].y = ly;
    pe[count].device = 0;
    pe[count].device_kind = kFlutterPointerDeviceKindMouse;
    pe[count].buttons = 0;
    target.pointer_added = true;
    ++count;
  }

  pe[count] = FlutterPointerEvent{};
  pe[count].struct_size = sizeof(FlutterPointerEvent);
  pe[count].phase = button_mask_ ? kMove : kHover;
  pe[count].timestamp = ts;
  pe[count].x = lx;
  pe[count].y = ly;
  pe[count].device = 0;
  pe[count].device_kind = kFlutterPointerDeviceKindMouse;
  pe[count].buttons = button_mask_;
  ++count;

  DispatchToRegion(target, pe, count);
}

void DrmSeat::HandlePointerButton(const drm::input::PointerButtonEvent& ev) {
  if (regions_.empty() || active_region_ >= regions_.size()) {
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

  // First button down starts the implicit grab on the view under the pointer.
  if (prev_mask == 0 && button_mask_ != 0) {
    grab_active_ = true;
    grab_region_ = active_region_;
  }

  FlutterPointerPhase phase;
  if (ev.pressed) {
    // A press while another button is already held becomes kMove; kDown is
    // reserved for the first-button-down transition.
    phase = (prev_mask == 0) ? kDown : kMove;
  } else {
    phase = (button_mask_ == 0) ? kUp : kMove;
  }

  // While grabbed, button events (including the release) go to the grabbed
  // view even if the cursor has drifted onto another display; else to the view
  // under the pointer. Coordinates are relative to that view.
  const size_t target_idx = (grab_active_ && grab_region_ < regions_.size())
                                ? grab_region_
                                : active_region_;
  const ViewRegion& region = regions_[target_idx];
  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = phase;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = pointer_x_ - region.layout_x;
  pe.y = pointer_y_ - region.layout_y;
  pe.device = 0;
  pe.device_kind = kFlutterPointerDeviceKindMouse;
  pe.buttons = button_mask_;

  DispatchToRegion(region, &pe, 1);

  // The grab ends when the last button releases. If the cursor drifted onto a
  // different display during the drag, clear the grabbed view's now-stale hover
  // so focus (and the next kAdd) can move to the display the pointer sits on.
  if (button_mask_ == 0 && grab_active_) {
    if (active_region_ != grab_region_ && grab_region_ < regions_.size() &&
        regions_[grab_region_].pointer_added) {
      ViewRegion& g = regions_[grab_region_];
      FlutterPointerEvent rm{};
      rm.struct_size = sizeof(FlutterPointerEvent);
      rm.phase = kRemove;
      rm.timestamp = FlutterTimestampMicros();
      rm.x = pointer_x_ - g.layout_x;
      rm.y = pointer_y_ - g.layout_y;
      rm.device = 0;
      rm.device_kind = kFlutterPointerDeviceKindMouse;
      rm.buttons = 0;
      DispatchToRegion(g, &rm, 1);
      g.pointer_added = false;
    }
    grab_active_ = false;
  }
}

void DrmSeat::HandlePointerAxis(const drm::input::PointerAxisEvent& ev) const {
  if (regions_.empty() || active_region_ >= regions_.size()) {
    return;
  }

  const size_t target_idx = (grab_active_ && grab_region_ < regions_.size())
                                ? grab_region_
                                : active_region_;
  const ViewRegion& region = regions_[target_idx];
  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = button_mask_ ? kMove : kHover;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = pointer_x_ - region.layout_x;
  pe.y = pointer_y_ - region.layout_y;
  pe.device = 0;
  pe.device_kind = kFlutterPointerDeviceKindMouse;
  pe.buttons = button_mask_;
  pe.signal_kind = kFlutterPointerSignalKindScroll;
  pe.scroll_delta_x = ev.horizontal;
  pe.scroll_delta_y = ev.vertical;

  DispatchToRegion(region, &pe, 1);
}

void DrmSeat::FlushTouchBatch() const {
  if (touch_batch_.empty() || touch_batch_region_ == nullptr) {
    touch_batch_.clear();
    touch_batch_region_ = nullptr;
    return;
  }
  DispatchToRegion(*touch_batch_region_, touch_batch_.data(),
                   touch_batch_.size());
  touch_batch_.clear();
  touch_batch_region_ = nullptr;
}

void DrmSeat::HandleTouch(const drm::input::TouchEvent& ev) const {
  if (ev.type == drm::input::TouchEvent::Type::Frame) {
    // Frame marker: everything since the previous marker is one hardware
    // scan — hand it to the engine as one batch.
    FlushTouchBatch();
    return;
  }
  if (regions_.empty()) {
    return;
  }
  // A touchscreen reports absolute coordinates in its own space with no cursor,
  // so it can't be routed by pointer position like a mouse. Route by device:
  // the region whose configured touch_match ([view.output] touch_device) is a
  // substring of this event's libinput device name. With none configured (or no
  // match) touch falls back to the primary region, preserving single-display
  // behavior.
  const ViewRegion* region = nullptr;
  if (ev.device_name != nullptr) {
    const std::string_view name(ev.device_name);
    for (const auto& r : regions_) {
      if (!r.touch_match.empty() &&
          name.find(r.touch_match) != std::string_view::npos) {
        region = &r;
        break;
      }
    }
  }
  if (region == nullptr) {
    region = &regions_.front();
  }
  if (region->state == nullptr || region->state->engine == nullptr) {
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

  // ev.x/ev.y are normalized [0,1) in the device's native orientation. Scale to
  // the render viewport, applying the INVERSE of the display rotation so a
  // touch lands under the finger on the rotated image (the cursor uses the
  // forward rotation; touch is its inverse).
  const double nx = ev.x;
  const double ny = ev.y;
  const auto w = static_cast<double>(region->width);
  const auto h = static_cast<double>(region->height);
  double fx = nx * w;
  double fy = ny * h;
  switch (region->cursor_rotation) {
    case 90:
      fx = (1.0 - ny) * w;
      fy = nx * h;
      break;
    case 180:
      fx = (1.0 - nx) * w;
      fy = (1.0 - ny) * h;
      break;
    case 270:
      fx = ny * w;
      fy = (1.0 - nx) * h;
      break;
    default:
      break;
  }

  // libinput touch-UP/CANCEL carry no coordinates (they default to 0,0, which
  // the transform maps to a corner and snaps the gesture there — breaking
  // swipes). Reuse the slot's last down/motion position instead; down/motion
  // record it.
  if (phase == kUp || phase == kCancel) {
    if (const auto it = touch_pos_.find(ev.slot); it != touch_pos_.end()) {
      fx = it->second.first;
      fy = it->second.second;
      touch_pos_.erase(it);
    }
  } else {
    touch_pos_[ev.slot] = {fx, fy};
  }

  FlutterPointerEvent pe{};
  pe.struct_size = sizeof(FlutterPointerEvent);
  pe.phase = phase;
  pe.timestamp = FlutterTimestampMicros();
  pe.x = fx;
  pe.y = fy;
  pe.device = ev.slot;
  pe.device_kind = kFlutterPointerDeviceKindTouch;
  pe.buttons = 0;

  // Contacts must not straddle regions within one batch (multi-panel setups
  // route by device); flush the previous region's batch first.
  if (touch_batch_region_ != nullptr && touch_batch_region_ != region) {
    FlushTouchBatch();
  }
  touch_batch_region_ = region;
  touch_batch_.push_back(pe);

  // Backstops: cancel ends the session with no guaranteed trailing frame
  // marker, and a runaway stream without markers must not grow unbounded.
  if (phase == kCancel ||
      touch_batch_.size() >= static_cast<size_t>(kMaxPointerEvent)) {
    FlushTouchBatch();
  }
}
}  // namespace homescreen
