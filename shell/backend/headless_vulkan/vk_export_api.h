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

// Registry backing the one exported shell symbol `ihs_vk_export_get_api`
// (defined in vk_export_api.cc). The active HeadlessVulkanBackend registers
// itself here at construction and unregisters at destruction; the C ABI entry
// points load the registered backend and delegate to it. When no
// headless-vulkan backend is active the symbol returns NULL, so a non-headless
// host is inert.

class HeadlessVulkanBackend;

namespace ihs {

// Publish/withdraw the active backend for the exported C ABI. Register replaces
// the current pointer; Unregister only clears it when it still equals @p b, so
// a stale destructor never clobbers a newer backend.
void RegisterVkExportBackend(HeadlessVulkanBackend* b);
void UnregisterVkExportBackend(HeadlessVulkanBackend* b);

}  // namespace ihs
