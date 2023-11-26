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

#include <cstdint>
#include <string>

class PlatformView {
 public:
  PlatformView(int32_t id,
               std::string viewType,
               int32_t direction,
               double width,
               double height);

  ~PlatformView();

  void Resize(double width, double height);

  [[nodiscard]] int32_t GetId() const { return id_; }

  std::string GetViewType() { return viewType_; }

  [[nodiscard]] int32_t GetDirection() const { return direction_; }

 private:
  int32_t id_;
  std::string viewType_;
  int32_t direction_;
  double width_;
  double height_;
};