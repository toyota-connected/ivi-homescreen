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

#include "display/idisplay.h"

// No-op IDisplay for the software backend. App::Loop expects an
// IDisplay to query refresh-rate / drive the event-source loop; with
// no Wayland or DRM event source we satisfy the interface with safe
// defaults. Refresh rate is the only field consumed by App::Loop's
// sleep math (frame_time = 1000 / refresh); 60 Hz keeps it stable.
class SoftwareDisplay final : public IDisplay {
 public:
  SoftwareDisplay(int32_t width, int32_t height, double refresh_rate_hz);
  ~SoftwareDisplay() override = default;

  void StartEvents() override {}
  void StopEvents() override {}
  [[nodiscard]] int PollEvents() const override { return 0; }

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
};
