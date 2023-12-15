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

namespace plugin_filament_view {
class Skybox {
 public:
  Skybox(void* parent,
         const std::string& flutter_assets_path,
         const flutter::EncodableMap& params);
  std::optional<int32_t> GetType() { return skyboxType_; }
  void Print(const char* tag);

  // Disallow copy and assign.
  Skybox(const Skybox&) = delete;
  Skybox& operator=(const Skybox&) = delete;

 private:
  void* parent_;
  const std::string& flutterAssetsPath_;

  std::string assetPath_;
  std::string url_;
  std::optional<bool> showSun_;
  std::optional<int32_t> skyboxType_;
};
}  // namespace plugin_filament_view