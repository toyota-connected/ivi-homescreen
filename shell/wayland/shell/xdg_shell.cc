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

#include "wayland/shell/xdg_shell.h"

#include <algorithm>

#include <wayland-client.h>

#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/proxy_impl.hpp>      // wl::construct

#include "logging/logging.h"

namespace ivi {

namespace {

namespace xc = xdg_shell::client;

class XdgShellSurface;

// Custom xdg_toplevel handler that forwards the configure `states` array — the
// shipped wl::XdgToplevelHandler drops it, so window-state flags (fullscreen /
// maximized / ...) would be lost. Bodies are defined after XdgShellSurface.
//
// Only OnConfigure (states) and OnClose are overridden — both are core events
// in every xdg-shell version. The version-gated events (configure_bounds,
// wm_capabilities) are intentionally left to the generated base's no-op
// defaults: not overriding them keeps the handler portable across every
// protocol version with zero behavior change (we don't act on them). An
// `override` of a gated event would instead fail to compile against a system
// xdg-shell.xml that predates it. Override one only when we need to handle it.
class XdgToplevelHandler final : public xc::CXdgToplevel<XdgToplevelHandler> {
 public:
  XdgShellSurface* app_ = nullptr;
  void OnConfigure(int32_t w, int32_t h, wl_array* states) override;
  void OnClose() override;
};

/**
 * @brief The xdg_surface + xdg_toplevel role for a single window. Acts as the
 * "App" the CRTP handlers delegate to (OnXdgSurfaceConfigure / OnToplevel*),
 * forwarding normalized geometry + window-state events through the WindowConfig
 * callbacks. The xdg_surface ack_configure is performed by the handler.
 */
class XdgShellSurface final : public ShellSurface {
 public:
  XdgShellSurface(wl::XdgWmBaseHandler& wm_base,
                  wl_surface* surface,
                  WindowConfig config)
      : config_(std::move(config)) {
    auto* surface_proxy = reinterpret_cast<wl_proxy*>(surface);

    if (!wl::SetupHandler(
            xdg_surface_,
            wl::construct<xc::xdg_surface_traits,
                          xc::xdg_wm_base_traits::Op::GetXdgSurface>(
                wm_base, surface_proxy))) {
      spdlog::error("[XdgShell] xdg_wm_base.get_xdg_surface failed");
      return;
    }
    xdg_surface_.Get()->app_ = this;

    if (!wl::SetupHandler(
            xdg_toplevel_,
            wl::construct<xc::xdg_toplevel_traits,
                          xc::xdg_surface_traits::Op::GetToplevel>(
                *xdg_surface_.Get()))) {
      spdlog::error("[XdgShell] xdg_surface.get_toplevel failed");
      return;
    }
    xdg_toplevel_.Get()->app_ = this;

    xdg_toplevel_.Get()->SetAppId(config_.app_id.c_str());
    xdg_toplevel_.Get()->SetTitle(config_.app_id.c_str());
    if (config_.fullscreen) {
      SetFullscreen(config_.output);
    }
  }

  void SetTitle(const std::string& title) override {
    if (xdg_toplevel_) {
      xdg_toplevel_.Get()->SetTitle(title.c_str());
    }
  }

  void SetFullscreen(wl_output* output) override {
    if (xdg_toplevel_) {
      xdg_toplevel_.Get()->SetFullscreen(reinterpret_cast<wl_proxy*>(output));
    }
  }

  // --- xdg CRTP handler callbacks (App contract) ---------------------------

  // ack_configure already sent by XdgSurfaceHandler before this fires.
  void OnXdgSurfaceConfigure(uint32_t /*serial*/) {}

  void OnToplevelConfigure(int32_t width,
                           int32_t height,
                           wl_array* states) const {
    // Always notify (even on a 0x0 "pick your own size" configure) so the
    // window can clear its wait-for-configure gate and refresh its state flags.
    SurfaceConfigure c;
    c.width = width;
    c.height = height;
    if (states != nullptr) {
      const auto* data = static_cast<const uint32_t*>(states->data);
      const std::size_t n = states->size / sizeof(uint32_t);
      for (std::size_t i = 0; i < n; ++i) {
        switch (static_cast<xc::XdgToplevelState>(data[i])) {
          case xc::XdgToplevelState::Fullscreen:
            c.fullscreen = true;
            break;
          case xc::XdgToplevelState::Maximized:
            c.maximized = true;
            break;
          case xc::XdgToplevelState::Resizing:
            c.resizing = true;
            break;
          case xc::XdgToplevelState::Activated:
            c.activated = true;
            break;
          default:
            break;
        }
      }
    }
    if (config_.on_configure) {
      config_.on_configure(c);
    }
  }

  void OnToplevelClose() const {
    if (config_.on_close) {
      config_.on_close();
    }
  }

 private:
  WindowConfig config_;
  wl::WlPtr<wl::XdgSurfaceHandler<XdgShellSurface>> xdg_surface_;
  wl::WlPtr<XdgToplevelHandler> xdg_toplevel_;
};

void XdgToplevelHandler::OnConfigure(int32_t w, int32_t h, wl_array* states) {
  if (app_ != nullptr) {
    app_->OnToplevelConfigure(w, h, states);
  }
}

void XdgToplevelHandler::OnClose() {
  if (app_ != nullptr) {
    app_->OnToplevelClose();
  }
}

}  // namespace

bool XdgShell::TryBindGlobal(wl_registry* registry,
                             const uint32_t name,
                             const std::string_view interface,
                             const uint32_t version) {
  if (interface != xc::xdg_wm_base_traits::wl_iface().name) {
    return false;
  }
  const uint32_t bind_ver = std::min(version, xc::xdg_wm_base_traits::version);
  auto* raw = wl_registry_bind(registry, name,
                               &xc::xdg_wm_base_traits::wl_iface(), bind_ver);
  if (!wl::SetupHandler(xdg_wm_base_, reinterpret_cast<wl_proxy*>(raw))) {
    spdlog::error("[XdgShell] failed to set up xdg_wm_base");
    return false;
  }
  spdlog::debug("[XdgShell] bound xdg_wm_base v{}", bind_ver);
  return true;
}

std::unique_ptr<ShellSurface> XdgShell::CreateSurface(
    wl_surface* surface,
    const WindowConfig& config) {
  if (!xdg_wm_base_) {
    spdlog::error("[XdgShell] CreateSurface called before xdg_wm_base bound");
    return nullptr;
  }
  return std::make_unique<XdgShellSurface>(*xdg_wm_base_.Get(), surface,
                                           config);
}

}  // namespace ivi
