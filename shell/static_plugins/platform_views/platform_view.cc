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

#include "platform_view.h"

#include "logging/logging.h"

PlatformView::PlatformView(const int32_t id,
                           std::string viewType,
                           const int32_t direction,
                           const double width,
                           const double height)
    : id_(id),
      viewType_(std::move(viewType)),
      direction_(direction),
      width_(width),
      height_(height) {
  SPDLOG_DEBUG("PlatformView:");
  SPDLOG_DEBUG("\tid: {}", id_);
  SPDLOG_DEBUG("\tviewType: {}", viewType);
  SPDLOG_DEBUG("\tdirection: {}", direction_);
  SPDLOG_DEBUG("\twidth: {}", width_);
  SPDLOG_DEBUG("\theight: {}", height_);
}

PlatformView::~PlatformView() = default;

void PlatformView::Resize(const double width, const double height) {
  SPDLOG_DEBUG("Resize [{}]:", viewType_);
  SPDLOG_DEBUG("\twidth: {}", width);
  SPDLOG_DEBUG("\theight: {}", height);

  width_ = width;
  height_ = height;
}
