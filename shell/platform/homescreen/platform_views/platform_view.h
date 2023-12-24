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

#include <string>

#include "platform_view_listener.h"

class PlatformView {
 public:
  PlatformView(const int32_t id,
               std::string viewType,
               const int32_t direction,
               const double width,
               const double height)
      : id_(id),
        viewType_(std::move(viewType)),
        direction_(direction),
        left_(0),
        top_(0),
        width_(static_cast<int32_t>(width)),
        height_(static_cast<int32_t>(height)) {}

  virtual ~PlatformView() = default;

  [[nodiscard]] std::pair<int32_t, int32_t> GetSize() const {
    return {width_, height_};
  }

  [[nodiscard]] std::pair<int32_t, int32_t> GetOffset() const {
    return {left_, top_};
  }

  [[nodiscard]] int32_t GetId() const { return id_; }

  std::string GetViewType() { return viewType_; }

  [[nodiscard]] int32_t GetDirection() const { return direction_; }

 private:
  int32_t id_;
  std::string viewType_;

 protected:
  int32_t direction_;
  int32_t left_, top_;
  int32_t width_, height_;
};