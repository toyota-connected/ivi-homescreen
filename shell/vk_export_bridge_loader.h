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

#include <string>

namespace ihs {

// Env-gated loader for an in-process ihs-vk-export bridge plugin. When
// IVI_VK_BRIDGE_SO names a shared module, this dlopen's it and drives its four
// C ABI entry points (create -> start ... stop -> destroy) so the module can
// forward the headless-vulkan backend's exported image pool to another process.
//
// The bridge resolves the shell's single exported `ihs_vk_export_get_api`
// symbol itself via dlsym(RTLD_DEFAULT), so this loader only has to bring the
// module in and manage its lifetime; it never touches the export API directly.
// The App owns one of these: it must be constructed AFTER the headless-vulkan
// backend has registered itself (so the export API resolves to a live backend)
// and destroyed BEFORE the backend, so the module's frame listener is cleared
// and its IO thread joined while the backend is still alive.
//
// Unless IVI_VK_BRIDGE_SO is set the loader is completely inert. Any failure
// (missing module, unresolved symbols, no active export) is logged and left
// non-fatal: an ordinary shell run is unaffected.
class VkExportBridgeLoader {
 public:
  VkExportBridgeLoader();
  ~VkExportBridgeLoader();

  VkExportBridgeLoader(const VkExportBridgeLoader&) = delete;
  VkExportBridgeLoader& operator=(const VkExportBridgeLoader&) = delete;

  // True once the module is loaded and its bridge instance started.
  [[nodiscard]] bool active() const { return bridge_ != nullptr; }

 private:
  using CreateFn = void* (*)(const char*);
  using StartFn = int (*)(void*);
  using StopFn = void (*)(void*);
  using DestroyFn = void (*)(void*);

  void* dl_ = nullptr;      // dlopen handle for the module
  void* bridge_ = nullptr;  // opaque IhsCarlaBridge* the module returned
  CreateFn create_ = nullptr;
  StartFn start_ = nullptr;
  StopFn stop_ = nullptr;
  DestroyFn destroy_ = nullptr;
};

}  // namespace ihs
