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

#include "plugins/common/common.h"

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

  return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
          static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f};
};

inline Color colorOf(const std::string& hexColorStr) {
  // remove '#' if present at the start
  const char* hexCode =
      hexColorStr[0] == '#' ? hexColorStr.c_str() + 1 : hexColorStr.c_str();

  unsigned long color =
      std::strtoul(hexCode, nullptr, 16);  // convert hex to decimal
  return colorOf(color);
}

/**
 * If rendering in linear space, first convert the gray scaled values to
 * linear space by rising to the power 2.2
 */
inline Color toLinearSpace(const Color& inputColor) {
  Color outputColor;
  outputColor.r = std::pow(inputColor.r, 2.2f);
  outputColor.g = std::pow(inputColor.g, 2.2f);
  outputColor.b = std::pow(inputColor.b, 2.2f);
  outputColor.a = inputColor.a;  // alpha remains the same
  return outputColor;
}

inline Color red(const Color& inputColor) {
  Color outputColor{};
  outputColor.r = inputColor.r;
  return outputColor;
}

inline Color green(const Color& inputColor) {
  Color outputColor{};
  outputColor.g = inputColor.g;
  return outputColor;
}

inline Color blue(const Color& inputColor) {
  Color outputColor{};
  outputColor.b = inputColor.b;
  return outputColor;
}

inline Color alpha(const Color& inputColor) {
  Color outputColor{};
  outputColor.a = inputColor.a;
  return outputColor;
}

inline Color toColor(::filament::float4 inputColor) {
  Color outputColor{};
  outputColor.r = inputColor[0];
  outputColor.g = inputColor[1];
  outputColor.b = inputColor[2];
  outputColor.r = inputColor[3] != 0 ? inputColor[3] : 1.0f;
  return outputColor;
}

}  // namespace plugin_filament_view
