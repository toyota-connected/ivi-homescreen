/*
 * Copyright 2020-2026 Toyota Connected North America
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

#ifndef SHELL_WAYLAND_SHELL_IVI_SHELL_H_
#define SHELL_WAYLAND_SHELL_IVI_SHELL_H_

#include "ivi_application_client.hpp"  // generated (ivi_application::client)

#include <wl/wl_ptr.hpp>

#include "wayland/shell/wayland_shell.h"

namespace ivi {

/// ivi_application has no events; the proxy is only used to mint ivi_surfaces.
class IviApplicationHandler final
    : public ivi_application::client::CIviApplication<IviApplicationHandler> {};

/**
 * @brief ivi-shell (ivi_application). Surfaces are given an ivi_surface role
 * keyed by a compositor-wide surface id (WindowConfig::ivi_surface_id), not an
 * xdg toplevel. ivi_wm (layer/visibility/z-order) is bound when advertised and
 * reserved for the window-management facet (OSGi bundle z-ordering); the role
 * itself only needs ivi_application.
 */
class IviShell final : public WaylandShell {
 public:
  IviShell() = default;

  [[nodiscard]] std::string_view Name() const override { return "ivi"; }

  [[nodiscard]] bool TryBindGlobal(wl_registry* registry,
                                   uint32_t name,
                                   std::string_view interface,
                                   uint32_t version) override;

  [[nodiscard]] bool IsBound() const override {
    return static_cast<bool>(ivi_app_);
  }

  [[nodiscard]] std::unique_ptr<ShellSurface> CreateSurface(
      wl_surface* surface,
      const WindowConfig& config) override;

 private:
  wl::WlPtr<IviApplicationHandler> ivi_app_;
};

}  // namespace ivi

#endif  // SHELL_WAYLAND_SHELL_IVI_SHELL_H_
