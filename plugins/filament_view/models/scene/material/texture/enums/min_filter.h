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

enum MinFilter {
  /// No filtering. Nearest neighbor is used.
  nearest,

  /// Box filtering. Weighted average of 4 neighbors is used.
  linear,

  /// Mip-mapping is activated. But no filtering occurs.
  nearestMipmapNearest,

  /// Box filtering within a mip-map level.
  linearMipmapNearest,

  /// Mip-map levels are interpolated, but no other filtering occurs.
  nearestMipmapLinear,

  /// Both interpolated Mip-mapping and linear filtering are used.
  linearMipmapLinear,
};

static constexpr char kMinFilterNearest[] = "NEAREST";
static constexpr char kMinFilterLinear[] = "LINEAR";
static constexpr char kMinFilterNearestMipmapNearest[] =
    "NEAREST_MIPMAP_NEAREST";
static constexpr char kMinFilterLinearMipmapNearest[] = "LINEAR_MIPMAP_NEAREST";
static constexpr char kMinFilterNearestMipmapLinear[] = "NEAREST_MIPMAP_LINEAR";
static constexpr char kMinFilterLinearMipmapLinear[] = "LINEAR_MIPMAP_LINEAR";

static const char* getTextForMinFilter(MinFilter min_filter) {
  return (const char*[]){
      kMinFilterNearest,
      kMinFilterLinear,
      kMinFilterNearestMipmapNearest,
      kMinFilterLinearMipmapNearest,
      kMinFilterNearestMipmapLinear,
      kMinFilterLinearMipmapLinear,
  }[min_filter];
}

}  // namespace plugin_filament_view::material::texture
