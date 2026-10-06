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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "display/idisplay.h"

class SoftwareCursor;

namespace homescreen {
class ISeat;
}  // namespace homescreen

// No-op IDisplay for the software backend. App::Loop expects an
// IDisplay to query refresh-rate / drive the event-source loop; with
// no Wayland or DRM event source we satisfy the interface with safe
// defaults. Refresh rate is the only field consumed by App::Loop's
// sleep math (frame_time = 1000 / refresh); 60 Hz keeps it stable.
class SoftwareDisplay final : public IDisplay {
 public:
  SoftwareDisplay(int32_t width, int32_t height, double refresh_rate_hz);
  ~SoftwareDisplay() override;

  // Optionally seed an input source at construction. Pass null when
  // running headless (CI memory/file sinks); on device sinks
  // (fbdev / drm-dumb) the SoftwareBackend installs a SoftwareSeat so
  // pointer / keyboard events drive Flutter.
  void SetSeat(std::unique_ptr<homescreen::ISeat> seat);

  // Resize the seat's pointer-clamp viewport to the backend's resolved size
  // (the sink's native mode). Mirrors DrmDisplay::SetViewportSize; called by
  // FlutterView once the SoftwareBackend has adopted the sink mode, so the
  // pointer coordinate space matches the framebuffer.
  void SetViewportSize(int32_t width, int32_t height);

  // Install the shared software cursor (created in app.cc when enabled). Stored
  // here so it outlives both the seat and the sink, and forwarded to the seat.
  // FlutterView reads cursor() to hand it to the backend's sink.
  void SetCursor(std::shared_ptr<SoftwareCursor> cursor);
  [[nodiscard]] const std::shared_ptr<SoftwareCursor>& cursor() const {
    return cursor_;
  }

  // Gate the input seat. When false, StartEvents() leaves the seat unstarted,
  // so libinput never opens /dev/input/event*. Default true. The leased-
  // software factory sets it false when a host Wayland session is present --
  // the leased path has no wl_surface, so its ungrabbed evdev reads would
  // duplicate input into both this process and the session compositor. Must be
  // called before StartEvents().
  void SetInputEnabled(bool enabled) { input_enabled_ = enabled; }

  void StartEvents() override;
  void StopEvents() override;
  [[nodiscard]] int PollEvents() override { return 0; }

  void SetViewControllerState(
      FlutterDesktopViewControllerState* state) override;

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
      int32_t device,
      const std::string& kind) const override;

  // The pointer from the application's own pixels, straight into the software
  // cursor's bitmap. SoftwareCursor::SetShape already takes premultiplied
  // ARGB8888 with a hotspot and copies under the lock BlendXRGB reads, so
  // there is nothing to queue here.
  //
  // False when there is no cursor at all, which is how the headless tiers
  // answer: MakeHeadlessEglDisplay and MakeHeadlessVulkanDisplay build a
  // SoftwareDisplay without calling SetCursor, so nothing would draw it.
  //
  // What this cannot see is the software tier pointed at an encoder sink
  // (IVI_ENC_SINK): the cursor exists and only DrmDumbSink composites it, so
  // the sprite is accepted and shows up nowhere. ActivateSystemCursor has the
  // same blind spot today -- the display does not know what its sink does --
  // and telling them apart needs the sink to say so.
  [[nodiscard]] bool SetCustomCursor(int32_t device,
                                     const std::vector<uint8_t>& pixels,
                                     int32_t width,
                                     int32_t height,
                                     int32_t hotspot_x,
                                     int32_t hotspot_y) override;

  [[nodiscard]] bool ClearCustomCursor(int32_t device) override;

  [[nodiscard]] bool HasRepeatTimer() const override { return false; }

  // wayland-leased-drm: everything the backend needs to build a DrmDumbSink on
  // a leased DRM fd, when this display was made for a lease.
  //
  // Deliberately not a LeaseHold: this header stays free of the lease client,
  // which the software tier otherwise has no reason to know about. The display
  // holds it because the descriptor's make_display/make_backend pair is the
  // only seam between negotiating the lease and building the sink -- and
  // because `owner` must outlive the sink either way.
  struct LeasedScanout {
    int fd = -1;                    // borrowed; owned by `owner`
    uint32_t connector_id = 0;      // the connector to drive, from the lease
    std::function<bool()> revoked;  // polled by the sink's Present() gate
    std::shared_ptr<void> owner;    // the LeaseHold; closes fd and returns the
                                    // lease when the display goes
  };
  void SetLeasedScanout(LeasedScanout scanout) {
    leased_scanout_ = std::move(scanout);
  }
  [[nodiscard]] const std::optional<LeasedScanout>& leased_scanout() const {
    return leased_scanout_;
  }

 private:
  int32_t width_;
  int32_t height_;
  double refresh_rate_hz_;
  FlutterDesktopViewControllerState* view_controller_state_ = nullptr;
  std::unique_ptr<homescreen::ISeat> seat_;
  // When false, StartEvents() leaves seat_ unstarted (see SetInputEnabled()).
  // Default true.
  bool input_enabled_ = true;
  std::shared_ptr<SoftwareCursor> cursor_;
  std::optional<LeasedScanout> leased_scanout_;
};
