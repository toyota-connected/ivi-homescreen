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

namespace plugin_filament_view::material::texture {

enum WrapMode {
  /// The edge of the texture extends to infinity.
  clampToEdge,

  /// The texture infinitely repeats in the wrap direction.
  repeat,

  /// The texture infinitely repeats and mirrors in the wrap direction.
  mirroredRepeat,
};

static constexpr char KWrapModeClampToEdge[] = "CLAMP_TO_EDGE";
static constexpr char KWrapModeRepeat[] = "REPEAT";
static constexpr char KWrapModeMirroredRepeat[] = "MIRRORED_REPEAT";

static const char* getTextForWrapMode(WrapMode mode) {
  return (const char*[]){
      KWrapModeClampToEdge,
      KWrapModeRepeat,
      KWrapModeMirroredRepeat,
  }[mode];
}

}  // namespace plugin_filament_view::material::texture
