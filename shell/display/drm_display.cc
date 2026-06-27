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

#include "display/drm_display.h"
#include "logging/logging.h"

#include <cstring>
#include <utility>

#include "backend/drm_kms_egl/drm_session.h"
#include "cursor_kind.h"
#include "display/icursor_shape_sink.h"
#include "input/drm_seat.h"
#include "logging.h"

namespace {

std::unique_ptr<homescreen::DrmSession> OpenSessionOrLog(bool no_seat) {
  if (no_seat) {
    spdlog::warn(
        "[DrmDisplay] --drm-no-seat: bypassing libseat — the backend opens "
        "/dev/dri + input directly and self-acquires DRM master, skipping "
        "the foreground-VT guard. No VT-switch / pause-resume; you are "
        "responsible for ensuring nothing else holds the display.");
    return nullptr;
  }
  auto session = homescreen::DrmSession::Open();
  if (!session) {
    spdlog::info(
        "[DrmDisplay] no libseat backend available — falling back to "
        "direct /dev/dri open + legacy VT/master guards");
  } else {
    spdlog::info("[DrmDisplay] libseat session active");
  }
  return session;
}

drm::input::InputDeviceOpener OpenerFrom(homescreen::DrmSession* session) {
  if (session == nullptr) {
    return {};
  }
  return session->InputOpener();
}

}  // namespace

DrmDisplay::DrmDisplay(int32_t width,
                       int32_t height,
                       double refresh_rate_hz,
                       bool no_seat)
    : width_(width),
      height_(height),
      refresh_rate_hz_(refresh_rate_hz),
      session_(OpenSessionOrLog(no_seat)),
      seat_(std::make_unique<homescreen::DrmSeat>(width,
                                                  height,
                                                  OpenerFrom(session_.get()))) {
  // Route Ctrl+Alt+F<n> through libseat. K_OFF (set inside DrmSeat) hides
  // the chord from the kernel keymap layer, so without this hook the
  // user has no way to leave the session short of remoting in and
  // running `chvt`. Only installed when both pieces exist — without a
  // libseat session there's no switch_session API to call.
  //
  // While we're here, also subscribe DrmSeat to session pause/resume
  // so libinput's evdev fds get suspended/resumed across the VT
  // round-trip. DrmBackend installs its own pair separately
  // (compositor pause + ScheduleFrame on resume).
  if (session_) {
    if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
      homescreen::DrmSession* sess = session_.get();
      drm_seat->SetVtSwitchHandler([sess](int vt) -> bool {
        const int err = sess->SwitchSession(vt);
        if (err != 0) {
          spdlog::warn("[DrmDisplay] SwitchSession({}) failed: {}", vt,
                       std::strerror(err));
          return false;
        }
        return true;
      });
      session_->AddPauseHandler([drm_seat]() { drm_seat->OnSessionPaused(); });
      session_->AddResumeHandler(
          [drm_seat](int /*fd*/) { drm_seat->OnSessionResumed(); });
    }
  }
}

DrmDisplay::~DrmDisplay() = default;

void DrmDisplay::StartEvents() {
  if (seat_) {
    seat_->Start();
  }
}

void DrmDisplay::StopEvents() {
  if (seat_) {
    seat_->Stop();
  }
}

void DrmDisplay::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  view_controller_state_ = state;
  if (seat_) {
    seat_->SetViewControllerState(state);
  }
}

void DrmDisplay::SetViewportSize(const int32_t width, const int32_t height) {
  width_ = width;
  height_ = height;
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetViewport(width, height);
  }
}

void DrmDisplay::SetCursor(homescreen::DrmCursor* cursor) {
  // The seat's polymorphic base ISeat has no cursor hook — that's
  // intentional, only the DRM seat drives a KMS cursor. Cast to the
  // concrete type and forward; the cast fails only if the build mixes
  // a non-DRM seat with this display, which doesn't happen today.
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetCursor(cursor);
  }
}

void DrmDisplay::SetGlCursor(homescreen::ICursorPositionSink* sink) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetGlCursor(sink);
  }
}

void DrmDisplay::SetCursorRotation(const int32_t degrees) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetCursorRotation(degrees);
  }
}

void DrmDisplay::SetCursorShapeSink(homescreen::ICursorShapeSink* sink) {
  shape_sink_ = sink;
}

bool DrmDisplay::ActivateSystemCursor(const int32_t /*device*/,
                                      const std::string& kind) const {
  if (shape_sink_ == nullptr) {
    return true;
  }
  return shape_sink_->SetShape(homescreen::CursorKindToXcursorName(kind));
}

void DrmDisplay::SetInputTransforms(const std::vector<std::string>& specs) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetInputTransforms(specs);
  }
}
