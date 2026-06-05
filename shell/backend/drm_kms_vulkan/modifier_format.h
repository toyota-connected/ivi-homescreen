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

namespace drm_kms_vulkan {

// Render a DRM format modifier as a readable label. Decodes AMD_FMT_MOD (tile
// version + swizzle mode + DCC flags), so a scanout layout is legible instead
// of a raw 64-bit blob — on AMD, "dcc=1" means the buffer is being scanned out
// Delta-Color-Compressed (the bandwidth win). LINEAR / INVALID are named;
// vendors we don't fully decode fall back to "<VENDOR>(0x..)". Depends only on
// <drm_fourcc.h> (header-only), so it is usable from the dependency-light
// probe.
std::string DescribeModifier(uint64_t modifier);

}  // namespace drm_kms_vulkan
