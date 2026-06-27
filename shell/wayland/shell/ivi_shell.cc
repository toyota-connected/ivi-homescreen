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

#include "wayland/shell/ivi_shell.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <wayland-client.h>

#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/proxy_impl.hpp>      // wl::construct_at_end

#include "logging/logging.h"

namespace ivi {

namespace {
namespace iac = ivi_application::client;

/// ivi_surface delivers a single configure(width, height) event.
class IviSurfaceHandler final : public iac::CIviSurface<IviSurfaceHandler> {
 public:
  std::function<void(int32_t, int32_t)> on_configure;

  void OnConfigure(int32_t width, int32_t height) override {
    if (on_configure) {
      on_configure(width, height);
    }
  }
};

/**
 * @brief ivi_surface role for one window. surface_create takes the compositor
 * surface id (WindowConfig::ivi_surface_id); the new ivi_surface object is the
 * trailing new_id, hence construct_at_end.
 */
class IviShellSurface final : public ShellSurface {
 public:
  IviShellSurface(IviApplicationHandler& app,
                  wl_surface* surface,
                  WindowConfig config)
      : config_(std::move(config)) {
    if (config_.ivi_surface_id == 0) {
      spdlog::warn(
          "[IviShell] ivi_surface_id is 0; ivi-shell needs an explicit "
          "surface id");
    }
    auto* surface_proxy = reinterpret_cast<wl_proxy*>(surface);
    auto* raw =
        wl::construct_at_end<iac::ivi_surface_traits,
                             iac::ivi_application_traits::Op::SurfaceCreate>(
            app, config_.ivi_surface_id, surface_proxy);
    if (!wl::SetupHandler(ivi_surface_, raw)) {
      spdlog::error("[IviShell] ivi_application.surface_create failed (id={})",
                    config_.ivi_surface_id);
      return;
    }
    ivi_surface_.Get()->on_configure = [this](int32_t w, int32_t h) {
      // ivi-shell has no window-state flags; report size only.
      if (config_.on_configure && (w != 0 || h != 0)) {
        SurfaceConfigure c;
        c.width = w;
        c.height = h;
        config_.on_configure(c);
      }
    };
    spdlog::debug("[IviShell] ivi_surface created (id={})",
                  config_.ivi_surface_id);
  }

 private:
  WindowConfig config_;
  wl::WlPtr<IviSurfaceHandler> ivi_surface_;
};

}  // namespace

bool IviShell::TryBindGlobal(wl_registry* registry,
                             const uint32_t name,
                             const std::string_view interface,
                             const uint32_t version) {
  if (interface == iac::ivi_application_traits::wl_iface().name) {
    const uint32_t bind_ver =
        std::min(version, iac::ivi_application_traits::version);
    auto* raw = wl_registry_bind(
        registry, name, &iac::ivi_application_traits::wl_iface(), bind_ver);
    ivi_app_.Attach(reinterpret_cast<wl_proxy*>(raw));  // no events on this iface
    spdlog::debug("[IviShell] bound ivi_application v{}", bind_ver);
    return true;
  }
  // ivi_wm (layer/visibility/z-order) is left for the window-management facet
  // (OSGi bundle z-ordering); binding it needs the generated ivi_wm header and
  // is deferred — the surface role only needs ivi_application.
  return false;
}

std::unique_ptr<ShellSurface> IviShell::CreateSurface(
    wl_surface* surface,
    const WindowConfig& config) {
  if (!ivi_app_) {
    spdlog::error("[IviShell] CreateSurface before ivi_application bound");
    return nullptr;
  }
  return std::make_unique<IviShellSurface>(*ivi_app_.Get(), surface, config);
}

}  // namespace ivi
