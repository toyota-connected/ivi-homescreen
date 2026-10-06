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
#include <string>
#include <utility>
#include <vector>

struct FlutterDesktopViewControllerState;

namespace homescreen {
class IOutputProvider;
}

class IDisplay {
 public:
  virtual ~IDisplay() = default;

  IDisplay(const IDisplay&) = delete;
  IDisplay& operator=(const IDisplay&) = delete;

  virtual void StartEvents() = 0;
  virtual void StopEvents() = 0;
  // Not const: the wayland implementation closes out a touch scan whose
  // wl_touch.frame never arrived once the dispatch round drains.
  [[nodiscard]] virtual int PollEvents() = 0;

  virtual void SetViewControllerState(
      FlutterDesktopViewControllerState* state) = 0;

  [[nodiscard]] virtual double GetRefreshRate(uint32_t index) const = 0;
  [[nodiscard]] virtual double GetMaxRefreshRate() const = 0;
  [[nodiscard]] virtual int32_t GetBufferScale(uint32_t index) const = 0;
  [[nodiscard]] virtual std::pair<int32_t, int32_t> GetVideoModeSize(
      uint32_t index) const = 0;

  [[nodiscard]] virtual bool ActivateSystemCursor(
      int32_t device,
      const std::string& kind) const = 0;

  /// A cursor drawn from pixels the application supplies, rather than a shape
  /// named out of the cursor theme.
  ///
  /// `activateSystemCursor` can only ask for the shapes a theme ships, and an
  /// application with its own art -- a game whose pointer is part of its look
  /// -- has no way to say so. The alternative is to hide the real cursor and
  /// draw one inside the toolkit, which costs a frame of latency and puts the
  /// pointer behind everything the compositor composites above the surface.
  ///
  /// [pixels] is ARGB8888, premultiplied, [width] * [height] * 4 bytes, rows
  /// top to bottom. The hotspot is in the same pixels.
  ///
  /// Not pure: a backend without a way to do this keeps the default and
  /// answers false, so adding one does not break the others.
  [[nodiscard]] virtual bool SetCustomCursor(
      int32_t /* device */,
      const std::vector<uint8_t>& /* pixels */,
      int32_t /* width */,
      int32_t /* height */,
      int32_t /* hotspot_x */,
      int32_t /* hotspot_y */) {
    return false;
  }

  /// Drops a custom cursor set earlier, back to the themed shape.
  [[nodiscard]] virtual bool ClearCustomCursor(int32_t /* device */) {
    return false;
  }

  [[nodiscard]] virtual bool HasRepeatTimer() const = 0;

  // The source of physical outputs for this display (the wl_registry on
  // Wayland, the card fd on DRM), or nullptr when the backend has no output
  // model. Borrowed — owned by the display.
  [[nodiscard]] virtual homescreen::IOutputProvider* GetOutputProvider() {
    return nullptr;
  }

 protected:
  IDisplay() = default;
};
