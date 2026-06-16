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

struct FlutterDesktopViewControllerState;

class IDisplay {
 public:
  virtual ~IDisplay() = default;

  IDisplay(const IDisplay&) = delete;
  IDisplay& operator=(const IDisplay&) = delete;

  virtual void StartEvents() = 0;
  virtual void StopEvents() = 0;
  [[nodiscard]] virtual int PollEvents() const = 0;

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

  [[nodiscard]] virtual bool HasRepeatTimer() const = 0;

 protected:
  IDisplay() = default;
};
