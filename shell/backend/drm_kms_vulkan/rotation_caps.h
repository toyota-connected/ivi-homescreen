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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What --drm-rotation needs from the display, decided before the first commit
// so an angle the hardware cannot do refuses at startup instead of scanning out
// unrotated (#838). Pure functions over plane rotation bits, modifiers and the
// KMS driver name, so they are testable without a display.
namespace drm_kms_vulkan {

// DRM_MODE_ROTATE_* for a rotation in degrees (validated to 0|90|180|270 at
// config time; anything else maps to ROTATE_0).
uint64_t RotationToDrmFlag(int degrees);

// Whether planes advertising `plane_bits` (DRM_MODE_ROTATE_* |
// DRM_MODE_REFLECT_*) can take `rotation_flag`. ROTATE_0 needs no rotation
// property at all, so it is always supported.
bool RotationSupported(uint64_t plane_bits, uint64_t rotation_flag);

// The bits as the kernel names them, e.g. "rotate-0,rotate-180,reflect-x", or
// "none" when there are none (no plane exposes a rotation property).
std::string DescribeRotationBits(uint64_t bits);

// Whether a KMS driver can rotate a LINEAR buffer by 90/270. amdgpu cannot:
// its rotated fetch needs a tiled read pattern. Every other driver is left to
// the kernel's own check at commit rather than predicted here.
bool LinearRotates(std::string_view kms_driver);

// Whether `modifier` can be scanned out rotated by 90/270 on `kms_driver`.
// LINEAR only where LinearRotates says so; never AMD DCC, which the rotated
// read is incompatible with.
bool RotationCompatible(uint64_t modifier, std::string_view kms_driver);

// The backing-store modifiers for a 90/270 rotation, from the negotiated set:
// the compatible tiled ones when there are any, else LINEAR when the driver
// rotates it. Empty means this display cannot serve the rotation.
std::vector<uint64_t> RotatedScanoutModifiers(
    const std::vector<uint64_t>& allowed,
    std::string_view kms_driver);

}  // namespace drm_kms_vulkan
