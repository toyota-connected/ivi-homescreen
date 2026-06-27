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

#ifndef SHELL_WAYLAND_SHELL_XDG_SHELL_H_
#define SHELL_WAYLAND_SHELL_XDG_SHELL_H_

#include "xdg_shell_client.hpp"  // generated proxies (xdg_shell::client)

#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>  // wl::XdgWmBaseHandler + interface tables

#include "wayland/shell/wayland_shell.h"

namespace ivi {

/**
 * @brief The standard XDG shell (xdg_wm_base). Binds the global, responds to
 * ping automatically (wl::XdgWmBaseHandler), and assigns the xdg_surface +
 * xdg_toplevel role to each window via XdgShellSurface.
 *
 * AGL reuses the xdg role on top of this; see AglShell.
 */
class XdgShell : public WaylandShell {
 public:
  XdgShell() = default;

  [[nodiscard]] std::string_view Name() const override { return "xdg"; }

  [[nodiscard]] bool TryBindGlobal(wl_registry* registry,
                                   uint32_t name,
                                   std::string_view interface,
                                   uint32_t version) override;

  [[nodiscard]] bool IsBound() const override {
    return static_cast<bool>(xdg_wm_base_);
  }

  [[nodiscard]] std::unique_ptr<ShellSurface> CreateSurface(
      wl_surface* surface,
      const WindowConfig& config) override;

  /// The bound xdg_wm_base handler, so AglShell (which reuses the xdg role) can
  /// share it instead of binding a second one.
  [[nodiscard]] wl::XdgWmBaseHandler* WmBase() { return xdg_wm_base_.Get(); }

 private:
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
};

}  // namespace ivi

#endif  // SHELL_WAYLAND_SHELL_XDG_SHELL_H_
