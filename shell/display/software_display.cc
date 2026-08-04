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

#include "display/software_display.h"

#include <cstdint>
#include <utility>

#include "config/common.h"  // BUILD_SOFTWARE_INPUT_LIBINPUT
#if BUILD_SOFTWARE_INPUT_LIBINPUT
#include "backend/software/input/software_seat.h"
#endif
#include "backend/software/software_cursor.h"
#include "cursor_kind.h"
#include "input/iseat.h"

#ifdef HAVE_SW_CURSOR_THEME
#include "cursor/cursor.hpp"
#include "cursor/theme.hpp"
#endif

SoftwareDisplay::SoftwareDisplay(const int32_t width,
                                 const int32_t height,
                                 const double refresh_rate_hz)
    : width_(width), height_(height), refresh_rate_hz_(refresh_rate_hz) {}

SoftwareDisplay::~SoftwareDisplay() = default;

void SoftwareDisplay::SetSeat(std::unique_ptr<homescreen::ISeat> seat) {
  seat_ = std::move(seat);
}

void SoftwareDisplay::SetViewportSize(const int32_t width,
                                      const int32_t height) {
  width_ = width;
  height_ = height;
  // SetViewport is SoftwareSeat-specific (not on ISeat), so downcast — the
  // only seat type a SoftwareDisplay ever holds. SoftwareSeat exists only with
  // the libinput seat; without it there is no seat to size.
#if BUILD_SOFTWARE_INPUT_LIBINPUT
  if (auto* sw_seat = dynamic_cast<homescreen::SoftwareSeat*>(seat_.get())) {
    sw_seat->SetViewport(width, height);
  }
#endif
}

void SoftwareDisplay::SetCursor(std::shared_ptr<SoftwareCursor> cursor) {
  cursor_ = std::move(cursor);
#if BUILD_SOFTWARE_INPUT_LIBINPUT
  if (auto* sw_seat = dynamic_cast<homescreen::SoftwareSeat*>(seat_.get())) {
    sw_seat->SetCursor(cursor_);
  }
#endif
}

bool SoftwareDisplay::ActivateSystemCursor(const int32_t /*device*/,
                                           const std::string& kind) const {
  if (!cursor_) {
    return true;
  }
  const char* const name = homescreen::CursorKindToXcursorName(kind);
  if (name == nullptr) {  // "none" — hide the cursor.
    cursor_->SetVisible(false);
    return true;
  }
#ifdef HAVE_SW_CURSOR_THEME
  // XCURSOR_SIZE convention is 24 logical px; the resolver scales to the
  // nearest available size. The theme is resolved once (process-global).
  constexpr uint32_t kThemeLogicalSize = 24;
  static auto theme = drm::cursor::Theme::discover();
  if (theme) {
    auto cursor =
        drm::cursor::Cursor::load(*theme, name, {}, kThemeLogicalSize);
    if (cursor) {
      const auto& f = cursor->first();
      cursor_->SetShape(f.pixels.data(), f.width, f.height, f.xhot, f.yhot);
      cursor_->SetVisible(true);
      return true;
    }
  }
#endif
  // No theme available or shape not found: keep the current sprite visible.
  cursor_->SetVisible(true);
  return true;
}

void SoftwareDisplay::StartEvents() {
  if (!input_enabled_) {
    // Seat left unstarted on purpose (SetInputEnabled(false)) — libinput never
    // opens /dev/input/event*. The leased-software factory does this when a
    // host Wayland session is present; the reason was logged there.
    return;
  }
  if (seat_) {
    seat_->Start();
  }
}

void SoftwareDisplay::StopEvents() {
  if (seat_) {
    // Null the seat's view-controller pointer before joining so any
    // in-flight dispatch sees the cleared state and bails out cleanly
    // rather than racing with engine teardown for the lambda chain.
    seat_->SetViewControllerState(nullptr);
    seat_->Stop();
  }
}

void SoftwareDisplay::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  view_controller_state_ = state;
  if (seat_) {
    seat_->SetViewControllerState(state);
  }
}
