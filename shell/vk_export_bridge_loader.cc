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

#include "vk_export_bridge_loader.h"

#include <dlfcn.h>

#include <cstdlib>

#include "logging/logging.h"

namespace ihs {

namespace {

// Resolve the bridge socket path: an explicit override, else the well-known
// leaf under $XDG_RUNTIME_DIR, else a /tmp fallback for bare environments.
std::string ResolveSocketPath() {
  if (const char* s = std::getenv("IVI_VK_BRIDGE_SOCKET"); s != nullptr && *s) {
    return s;
  }
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR");
      xdg != nullptr && *xdg) {
    return std::string(xdg) + "/ihs-vk-export.sock";
  }
  return "/tmp/ihs-vk-export.sock";
}

}  // namespace

VkExportBridgeLoader::VkExportBridgeLoader() {
  const char* module = std::getenv("IVI_VK_BRIDGE_SO");
  if (module == nullptr || *module == '\0') {
    return;  // not configured; stay inert.
  }

  dl_ = ::dlopen(module, RTLD_NOW | RTLD_LOCAL);
  if (dl_ == nullptr) {
    ihs::log::error("[VkExportBridge] dlopen('{}') failed: {}", module,
                    ::dlerror());
    return;
  }

  create_ = reinterpret_cast<CreateFn>(::dlsym(dl_, "ihs_carla_bridge_create"));
  start_ = reinterpret_cast<StartFn>(::dlsym(dl_, "ihs_carla_bridge_start"));
  stop_ = reinterpret_cast<StopFn>(::dlsym(dl_, "ihs_carla_bridge_stop"));
  destroy_ =
      reinterpret_cast<DestroyFn>(::dlsym(dl_, "ihs_carla_bridge_destroy"));
  if (create_ == nullptr || start_ == nullptr || stop_ == nullptr ||
      destroy_ == nullptr) {
    ihs::log::error("[VkExportBridge] '{}' missing ihs_carla_bridge_* symbols",
                    module);
    ::dlclose(dl_);
    dl_ = nullptr;
    return;
  }

  const std::string socket_path = ResolveSocketPath();
  bridge_ = create_(socket_path.c_str());
  if (bridge_ == nullptr) {
    ihs::log::error("[VkExportBridge] create('{}') returned null", socket_path);
    ::dlclose(dl_);
    dl_ = nullptr;
    return;
  }

  if (start_(bridge_) != 0) {
    // The most common cause is that the active backend is not headless-vulkan
    // (or export is not enabled), so the module resolved no export API.
    ihs::log::error(
        "[VkExportBridge] start failed; no headless-vulkan export active");
    destroy_(bridge_);
    bridge_ = nullptr;
    ::dlclose(dl_);
    dl_ = nullptr;
    return;
  }

  ihs::log::info("[VkExportBridge] bridge started on '{}' (module '{}')",
                 socket_path, module);
}

VkExportBridgeLoader::~VkExportBridgeLoader() {
  if (bridge_ != nullptr) {
    stop_(bridge_);
    destroy_(bridge_);
    bridge_ = nullptr;
  }
  if (dl_ != nullptr) {
    ::dlclose(dl_);
    dl_ = nullptr;
  }
}

}  // namespace ihs
