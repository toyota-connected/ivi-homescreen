/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include <flutter/encodable_value.h>
#include <shell/platform/embedder/embedder.h>

#include "engine.h"

class PlatformViewTouch {
 public:
  explicit PlatformViewTouch(
      const std::vector<flutter::EncodableValue>& params);

  int32_t getId() const { return id_; }

  int32_t getAction() const { return action_; }

  double getX() { return rawPointerCoords_[7]; }

  double getY() { return rawPointerCoords_[8]; }

  void Print();

 private:
  /// The ID of the platform view as seen by the Flutter side.
  int32_t id_;
  /// The amount of time that the touch has been pressed.
  int32_t downTime_;
  int32_t eventTime_;
  int32_t action_;
  /// The number of pointers (e.g, fingers) involved in the touch event.
  int32_t pointerCount_;
  /// Properties for each pointer, encoded in a raw format.
  std::vector<int32_t> rawPointerPropertiesList_;
  /// Coordinates for each pointer, encoded in a raw format.
  std::vector<double> rawPointerCoords_;
  int32_t metaState_;
  int32_t buttonState_;
  /// Coordinate precision along the x-axis.
  double xPrecision_;
  /// Coordinate precision along the y-axis.
  double yPrecision_;
  int32_t deviceId_;
  int32_t edgeFlags_;
  int32_t source_;
  int32_t flags_;
  int32_t motionEventId_;
};
