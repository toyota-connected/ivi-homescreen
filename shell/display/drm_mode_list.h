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

// `--drm-list-modes` support, shared verbatim by the drm_kms_egl and
// drm_kms_vulkan backends (it only reads connector modes + plane properties via
// libdrm — no GL/Vulkan/GBM dependency). Read-only: opens the device, prints
// every connector's modes and each plane's rotation capability, and returns.

// Friendly connector-type name (libdrm's drmModeGetConnectorTypeName can return
// NULL on older libdrm, so we keep our own table).
const char* ConnectorTypeName(uint32_t type);

// Print all connectors + their modes, plus a per-plane rotation-capability
// section, for `--drm-list-modes`. Returns 0 on success, non-zero on open /
// resource-query failure.
int PrintDrmModes(const std::string& device);
