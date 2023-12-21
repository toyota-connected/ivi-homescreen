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

#include <string>

#include <math/vec4.h>

#include "logging/logging.h"

namespace plugin_filament_view {
using Color = ::filament::math::float4;

inline Color colorOf(float r = 0.0f,
                     float g = 0.0f,
                     float b = 0.0f,
                     float a = 1.0f) {
  return {r, g, b, a};
}

inline Color colorOf(float rgb = 0.0f, float a = 1.0f) {
  return colorOf(rgb, rgb, rgb, a);
}

inline Color colorOf(unsigned long color) {
  int a = (color >> 24) & 0xff;  // or color >>> 24
  int r = (color >> 16) & 0xff;
  int g = (color >> 8) & 0xff;
  int b = (color) & 0xff;

  SPDLOG_DEBUG("r: 0x{:x} - 0x{:x}", r, static_cast<float>(r) / 255.0f);
  SPDLOG_DEBUG("g: 0x{:x} - 0x{:x}", g, static_cast<float>(g) / 255.0f);
  SPDLOG_DEBUG("b: 0x{:x} - 0x{:x}", b, static_cast<float>(b) / 255.0f);
  SPDLOG_DEBUG("a: 0x{:x} - 0x{:x}", a, static_cast<float>(a) / 255.0f);

  return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
          static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f};
};

inline Color colorOf(std::string colorStr) {
  SPDLOG_DEBUG("colorOf: {}", colorStr);
  auto color = std::stoul(colorStr, 0, 16);
  SPDLOG_DEBUG("color: 0x{:x}", color);
  return colorOf(color);
}

#if 0
    fun FloatArray.toColor() =
        Color(this[0], this[1], this[2], this.getOrNull(3) ?: 1.0f)

    /**
     * If rendering in linear space, first convert the gray scaled values to
     * linear space by rising to the power 2.2
     */
    fun Color.toLinearSpace() = transform{pow(it, 2.2f)}

    fun Color.red() = this[0];
    fun Color.green() = this[1];
    fun Color.blue() = this[2];
    fun Color.alpha() = this[3]
#endif

}  // namespace plugin_filament_view
