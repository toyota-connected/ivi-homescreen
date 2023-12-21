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

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

namespace plugin_filament_view {

class TextureSampler {
 public:
  TextureSampler(const flutter::EncodableMap& params);

  void Print(const char* tag);

  // Disallow copy and assign.
  TextureSampler(const TextureSampler&) = delete;

  TextureSampler& operator=(const TextureSampler&) = delete;

 private:
  std::string min_;
  std::string mag_;
  std::string wrap_;
  std::optional<double> anisotropy_;
};
}  // namespace plugin_filament_view