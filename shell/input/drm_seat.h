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
#include <thread>

#include <drm-cxx/input/keyboard.hpp>
#include <drm-cxx/input/seat.hpp>

#include <shell/platform/embedder/embedder.h>

#include "backend/drm_kms_egl/session_watchdog.h"
#include "input/iseat.h"

namespace homescreen {

// libinput-backed seat. Pumps drm::input::Seat events on a dedicated thread
// and translates them into FlutterPointerEvent / FlutterKeyEvent. Events
// received before the Flutter engine is up are dropped (the state pointer
// resolves to a null engine handle in that window).
class DrmSeat final : public ISeat {
 public:
  DrmSeat(int32_t viewport_width, int32_t viewport_height);
  ~DrmSeat() override;

  bool Start() override;
  void Stop() override;
  void SetViewControllerState(
      FlutterDesktopViewControllerState* state) override;

 private:
  void DispatchLoop() const;
  void HandleEvent(const drm::input::InputEvent& ev);
  void HandleKeyboard(const drm::input::KeyboardEvent& ev) const;
  void HandlePointerMotion(const drm::input::PointerMotionEvent& ev);
  void HandlePointerButton(const drm::input::PointerButtonEvent& ev);
  void HandlePointerAxis(const drm::input::PointerAxisEvent& ev) const;
  void HandleTouch(const drm::input::TouchEvent& ev) const;

  [[nodiscard]] FLUTTER_API_SYMBOL(FlutterEngine) CurrentEngine() const;

  const int32_t viewport_w_;
  const int32_t viewport_h_;

  std::atomic<FlutterDesktopViewControllerState*> state_{nullptr};

  std::unique_ptr<drm::input::Seat> seat_;
  std::unique_ptr<drm::input::Keyboard> keyboard_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  // VT keyboard mode. In a text console, without K_OFF keystrokes are
  // also consumed by the kernel tty line discipline (echoed to the
  // underlying shell). K_OFF suppresses that; xkbcommon translation of
  // libinput events is unaffected and happens via keyboard_.
  int tty_fd_ = -1;
  int saved_kb_mode_ = 0;

  // Reverse-watchdog that restores saved_kb_mode_ if the parent dies via
  // SIGKILL (or any other path that skips Stop()). See session_watchdog.h.
  homescreen::watchdog::Handle tty_watchdog_{};

  // Accessed only from the dispatch thread.
  double pointer_x_ = 0.0;
  double pointer_y_ = 0.0;
  int64_t button_mask_ = 0;
  bool pointer_added_ = false;
};

}  // namespace homescreen
