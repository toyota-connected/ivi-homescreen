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

#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;

class Position;

class GlbLoader {
 public:
  explicit GlbLoader(CustomModelViewer* modelViewer,
                     const std::string& flutterAssets);

  ~GlbLoader() = default;

  std::future<std::string> loadGlbFromAsset(const std::string& path,
                                            float scale,
                                            const Position* centerPosition,
                                            bool isFallback = false);

  std::future<std::string> loadGlbFromUrl(const std::string& url,
                                          float scale,
                                          const Position* centerPosition,
                                          bool isFallback = false);

 private:
  CustomModelViewer* model_viewer_;
  std::string flutterAssets_;
  const asio::io_context::strand& strand_;
};
}  // namespace plugin_filament_view
