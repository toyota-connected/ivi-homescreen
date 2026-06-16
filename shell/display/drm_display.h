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

#include <memory>
#include <string>
#include <vector>

#include "display/idisplay.h"
#include "input/iseat.h"

namespace homescreen {
class DrmCursor;
class ICursorPositionSink;
class DrmSession;
}  // namespace homescreen

class DrmDisplay final : public IDisplay {
 public:
  // @p no_seat (--drm-no-seat / HOMESCREEN_DRM_NO_SEAT) skips opening a
  // libseat session entirely, forcing the backend's direct-open path. See
  // session().
  DrmDisplay(int32_t width,
             int32_t height,
             double refresh_rate_hz,
             bool no_seat = false);
  ~DrmDisplay() override;

  // Process-wide libseat session. Null when no seat backend is available
  // (no logind/seatd/builtin), when drm-cxx was built without libseat, or
  // when --drm-no-seat forced the direct-open path.
  // DrmBackend consults this to route its DRM device open through the
  // seat; when null, the backend falls back to direct open + the legacy
  // VT/master guards.
  [[nodiscard]] homescreen::DrmSession* session() const {
    return session_.get();
  }

  void StartEvents() override;
  void StopEvents() override;
  [[nodiscard]] int PollEvents() const override { return 0; }

  void SetViewControllerState(
      FlutterDesktopViewControllerState* state) override;

  // Forward the DRM hardware cursor to the seat thread so pointer
  // events move the on-screen sprite. nullptr is safe and disables
  // the sprite (used during teardown before DrmBackend destroys the
  // DrmCursor it owns).
  void SetCursor(homescreen::DrmCursor* cursor);

  // Forward the composited cursor sink (the EGL backend's GlCursor, used when
  // there's no HW cursor plane) to the seat. nullptr is safe.
  void SetGlCursor(homescreen::ICursorPositionSink* sink);

  // Forward the scanout rotation to the seat so the HW cursor sprite is
  // transformed from render space into panel space (0|90|180|270).
  void SetCursorRotation(int32_t degrees);

  // Forward per-device relative-pointer transforms to the seat
  // ("<name>=<rot>[,flip-x][,flip-y]"; see DrmSeat::SetInputTransforms).
  void SetInputTransforms(const std::vector<std::string>& specs);

  // Update the seat's cursor clamping rectangle to match the actual
  // backend framebuffer size. App::MakeDisplay constructs DrmDisplay
  // with the config's view.width/height (defaults 1024x768) — but
  // when `-f` is passed the backend FB is promoted to the full mode
  // dimensions. FlutterView calls this after DrmBackend::Create
  // resolves the real width/height so cursor motion clamps to the
  // visible area rather than the stale config size.
  void SetViewportSize(int32_t width, int32_t height);

  [[nodiscard]] double GetRefreshRate(uint32_t /*index*/) const override {
    return refresh_rate_hz_;
  }
  [[nodiscard]] double GetMaxRefreshRate() const override {
    return refresh_rate_hz_;
  }
  [[nodiscard]] int32_t GetBufferScale(uint32_t /*index*/) const override {
    return 1;
  }
  [[nodiscard]] std::pair<int32_t, int32_t> GetVideoModeSize(
      uint32_t /*index*/) const override {
    return {width_, height_};
  }

  [[nodiscard]] bool ActivateSystemCursor(
      int32_t /*device*/,
      const std::string& /*kind*/) const override {
    return true;
  }

  [[nodiscard]] bool HasRepeatTimer() const override { return false; }

 private:
  int32_t width_;
  int32_t height_;
  double refresh_rate_hz_;
  FlutterDesktopViewControllerState* view_controller_state_ = nullptr;

  // Seat session must outlive seat_ (DrmSeat's libinput_opener captures
  // into the session's internal state). Declare it first so it's
  // destroyed last.
  std::unique_ptr<homescreen::DrmSession> session_;

  // Input source. Defaults to a libinput-backed DrmSeat; the polymorphism
  // is there so a Wayland-client + DRM-rendering configuration can swap in
  // a WaylandSeat without changing this class.
  std::unique_ptr<homescreen::ISeat> seat_;
};
