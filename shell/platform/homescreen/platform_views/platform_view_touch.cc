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

#include "platform_view_touch.h"

PlatformViewTouch::PlatformViewTouch(
    const std::vector<flutter::EncodableValue>& params) {
#if DEBUG_PLATFORM_VIEW_TOUCH_PARAMS
  for (const auto& it : params) {
    plugin_common::Encodable::PrintFlutterEncodableValue("PlatformViewTouch",
                                                          it);
  }
#endif
  if (std::holds_alternative<int32_t>(params[0])) {
    id_ = std::get<int32_t>(params[0]);
  }
  if (std::holds_alternative<int64_t>(params[1])) {
    downTime_ = std::get<int64_t>(params[1]);
  }
  if (std::holds_alternative<int64_t>(params[2])) {
    eventTime_ = std::get<int64_t>(params[2]);
  }
  if (std::holds_alternative<int32_t>(params[3])) {
    action_ = std::get<int32_t>(params[3]);
  }
  if (std::holds_alternative<int32_t>(params[4])) {
    pointerCount_ = std::get<int32_t>(params[4]);
  }
  if (std::holds_alternative<flutter::EncodableList>(params[5])) {
    if (const auto l1 = std::get<flutter::EncodableList>(params[5]);
        std::holds_alternative<flutter::EncodableList>(l1[0])) {
      for (const auto& it_ : std::get<flutter::EncodableList>(l1[0])) {
        rawPointerPropertiesList_.emplace_back(std::get<int32_t>(it_));
      }
    }
  }
  if (std::holds_alternative<flutter::EncodableList>(params[6])) {
    if (const auto l2 = std::get<flutter::EncodableList>(params[6]);
        std::holds_alternative<flutter::EncodableList>(l2[0])) {
      for (const auto& it_ : std::get<flutter::EncodableList>(l2[0])) {
        rawPointerCoords_.emplace_back(std::get<double>(it_));
      }
    }
  }
  if (std::holds_alternative<int32_t>(params[7])) {
    metaState_ = std::get<int32_t>(params[7]);
  }
  if (std::holds_alternative<int32_t>(params[8])) {
    buttonState_ = std::get<int32_t>(params[8]);
  }
  if (std::holds_alternative<double>(params[9])) {
    xPrecision_ = std::get<double>(params[9]);
  }
  if (std::holds_alternative<double>(params[10])) {
    yPrecision_ = std::get<double>(params[10]);
  }
  if (std::holds_alternative<int32_t>(params[11])) {
    deviceId_ = std::get<int32_t>(params[11]);
  }
  if (std::holds_alternative<int32_t>(params[12])) {
    edgeFlags_ = std::get<int32_t>(params[12]);
  }
  if (std::holds_alternative<int32_t>(params[13])) {
    source_ = std::get<int32_t>(params[13]);
  }
  if (std::holds_alternative<int32_t>(params[14])) {
    flags_ = std::get<int32_t>(params[14]);
  }
  if (std::holds_alternative<int32_t>(params[15])) {
    motionEventId_ = std::get<int32_t>(params[15]);
  }
}

void PlatformViewTouch::Print() {
  SPDLOG_DEBUG("[{}] pointerCount: {}, action: {}, raw x: {}, raw y: {}", id_,
               pointerCount_, action_, rawPointerCoords_[7],
               rawPointerCoords_[8]);
}
