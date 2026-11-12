/*
 * Copyright 2026 Toyota Connected North America
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

#include "backend/drm_kms_vulkan/rotation_caps.h"

#include <drm_fourcc.h>
#include <drm_mode.h>  // DRM_MODE_ROTATE_*, DRM_MODE_REFLECT_*

namespace drm_kms_vulkan {

uint64_t RotationToDrmFlag(const int degrees) {
  switch (degrees) {
    case 90:
      return DRM_MODE_ROTATE_90;
    case 180:
      return DRM_MODE_ROTATE_180;
    case 270:
      return DRM_MODE_ROTATE_270;
    default:
      return DRM_MODE_ROTATE_0;
  }
}

bool RotationSupported(const uint64_t plane_bits,
                       const uint64_t rotation_flag) {
  if (rotation_flag == DRM_MODE_ROTATE_0) {
    return true;
  }
  return (plane_bits & rotation_flag) == rotation_flag;
}

std::string DescribeRotationBits(const uint64_t bits) {
  static constexpr struct {
    uint64_t bit;
    const char* name;
  } kNames[] = {
      {DRM_MODE_ROTATE_0, "rotate-0"},     {DRM_MODE_ROTATE_90, "rotate-90"},
      {DRM_MODE_ROTATE_180, "rotate-180"}, {DRM_MODE_ROTATE_270, "rotate-270"},
      {DRM_MODE_REFLECT_X, "reflect-x"},   {DRM_MODE_REFLECT_Y, "reflect-y"},
  };
  std::string out;
  for (const auto& n : kNames) {
    if ((bits & n.bit) != 0U) {
      if (!out.empty()) {
        out += ',';
      }
      out += n.name;
    }
  }
  return out.empty() ? "none" : out;
}

bool LinearRotates(const std::string_view kms_driver) {
  return kms_driver != "amdgpu";
}

bool RotationCompatible(const uint64_t modifier,
                        const std::string_view kms_driver) {
  if (modifier == DRM_FORMAT_MOD_LINEAR) {
    return LinearRotates(kms_driver);
  }
#ifdef AMD_FMT_MOD
  if (static_cast<uint8_t>(modifier >> 56) == DRM_FORMAT_MOD_VENDOR_AMD &&
      AMD_FMT_MOD_GET(DCC, modifier) != 0U) {
    return false;
  }
#endif
  return true;
}

std::vector<uint64_t> RotatedScanoutModifiers(
    const std::vector<uint64_t>& allowed,
    const std::string_view kms_driver) {
  std::vector<uint64_t> tiled;
  bool linear = false;
  for (const auto m : allowed) {
    if (!RotationCompatible(m, kms_driver)) {
      continue;
    }
    if (m == DRM_FORMAT_MOD_LINEAR) {
      linear = true;
    } else {
      tiled.push_back(m);
    }
  }
  // Tiled first, as before: a driver that can rotate LINEAR may still read a
  // rotated tiled buffer more efficiently, and it is what amdgpu needs.
  if (!tiled.empty()) {
    return tiled;
  }
  if (linear) {
    return {DRM_FORMAT_MOD_LINEAR};
  }
  return {};
}

}  // namespace drm_kms_vulkan
