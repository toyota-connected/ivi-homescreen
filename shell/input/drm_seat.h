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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <drm-cxx/input/key_repeater.hpp>
#include <drm-cxx/input/keyboard.hpp>
#include <drm-cxx/input/seat.hpp>

#include <shell/platform/embedder/embedder.h>

#include "input/iseat.h"

namespace homescreen {

class DrmCursor;

// Owned-string variant of drm::input::KeymapOptions, safe to pass
// across threads. drm::input::KeymapOptions itself uses string_views,
// which can't survive cross-thread hand-off.
struct KeymapConfig {
  std::string rules;
  std::string model;
  std::string layout;
  std::string variant;
  std::string options;
};

// libinput-backed seat. Pumps drm::input::Seat events on a dedicated thread
// and translates them into FlutterPointerEvent / FlutterKeyEvent. Events
// received before the Flutter engine is up are dropped (the state pointer
// resolves to a null engine handle in that window).
//
// When a libseat-backed session is live, the caller passes an
// InputDeviceOpener (from DrmSession::InputOpener()) so libinput's
// privileged /dev/input/event* opens route through libseat — giving
// input fds the same revocable lifetime as the DRM fd. In that mode,
// the K_OFF TTY keyboard-mode hack is skipped; logind/seatd manages
// VT keyboard state on session activation.
class DrmSeat final : public ISeat {
 public:
  DrmSeat(int32_t viewport_width,
          int32_t viewport_height,
          drm::input::InputDeviceOpener opener = {});
  ~DrmSeat() override;

  bool Start() override;
  void Stop() override;
  void SetViewControllerState(
      FlutterDesktopViewControllerState* state) override;

  // Set (or clear) the DRM hardware cursor this seat moves. Safe to
  // call from any thread; the dispatch loop reads via acquire and
  // ignores nullptr. Caller must clear (pass nullptr) before the
  // pointed-to DrmCursor is destroyed.
  void SetCursor(DrmCursor* cursor) {
    cursor_.store(cursor, std::memory_order_release);
  }

  // Hot-swap the xkb keymap. Safe to call from any thread; the actual
  // reload runs on the dispatch thread on the next poll iteration.
  // Held keys and lock latches are preserved across the swap. On
  // failure (bad RMLVO names), the existing keymap is left intact and
  // a warning is logged. Empty fields fall back to xkb's system
  // defaults for that knob. Currently dormant — no caller wires this;
  // intended for a future settings/locale channel.
  void ReloadKeymap(KeymapConfig cfg);

 private:
  void DispatchLoop();
  void ApplyPendingKeymap();
  void HandleEvent(const drm::input::InputEvent& ev);
  void HandleKeyboard(const drm::input::KeyboardEvent& ev);
  void DispatchKeyToFlutter(const drm::input::KeyboardEvent& resolved) const;
  void HandlePointerMotion(const drm::input::PointerMotionEvent& ev);
  void HandlePointerButton(const drm::input::PointerButtonEvent& ev);
  void HandlePointerAxis(const drm::input::PointerAxisEvent& ev) const;
  void HandleTouch(const drm::input::TouchEvent& ev) const;

  [[nodiscard]] FLUTTER_API_SYMBOL(FlutterEngine) CurrentEngine() const;

  const int32_t viewport_w_;
  const int32_t viewport_h_;

  std::atomic<FlutterDesktopViewControllerState*> state_{nullptr};

  // Caller-supplied hook for libinput's privileged device opens. Empty
  // when there's no libseat session (falls back to ::open/::close inside
  // drm::input::Seat).
  drm::input::InputDeviceOpener opener_;

  std::unique_ptr<drm::input::Seat> seat_;
  std::unique_ptr<drm::input::Keyboard> keyboard_;
  // Synthesizes auto-repeat KeyboardEvents for held keys. Absent when
  // disabled via IVI_DRM_KEY_REPEAT=0 or when KeyRepeater::create fails.
  std::optional<drm::input::KeyRepeater> repeater_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  // Keymap reloads posted from any thread, drained on the dispatch
  // thread by ApplyPendingKeymap() at the top of each DispatchLoop
  // iteration. Owns its strings so caller threads don't have to keep
  // them alive past ReloadKeymap() returning.
  std::mutex pending_mu_;
  std::optional<KeymapConfig> pending_keymap_;

  // VT keyboard mode. In a text console, without K_OFF keystrokes are
  // also consumed by the kernel tty line discipline (echoed to the
  // underlying shell). K_OFF suppresses that; xkbcommon translation of
  // libinput events is unaffected and happens via keyboard_.
  int tty_fd_ = -1;
  int saved_kb_mode_ = 0;

  // Accessed only from the dispatch thread.
  double pointer_x_ = 0.0;
  double pointer_y_ = 0.0;
  int64_t button_mask_ = 0;
  bool pointer_added_ = false;

  // KMS hardware cursor target. Owned by DrmBackend; this seat just
  // forwards motion to it. Atomic because the wiring runs on a
  // different thread than the dispatch loop that reads it.
  std::atomic<DrmCursor*> cursor_{nullptr};
};

}  // namespace homescreen
