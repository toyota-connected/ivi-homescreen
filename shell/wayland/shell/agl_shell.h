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

#ifndef SHELL_WAYLAND_SHELL_AGL_SHELL_H_
#define SHELL_WAYLAND_SHELL_AGL_SHELL_H_

#include <functional>

#include "agl_shell_client.hpp"  // generated proxies (agl_shell::client)

#include <wl/agl_shell.hpp>  // wl::AglShellHandler + interface tables
#include <wl/wl_ptr.hpp>

#include "wayland/shell/wayland_shell.h"
#include "wayland/shell/xdg_shell.h"

namespace ivi {

/**
 * @brief AGL shell. AGL surfaces use the standard xdg role, so AglShell extends
 * XdgShell and adds the agl_shell extension on top: the bound_ok/bound_fail
 * handshake (Sync), agl_shell.ready (OnClientReady), and the AGL-only
 * window-management facet (set_background / set_panel / activation region).
 *
 * Degrades gracefully: when the compositor does not advertise agl_shell (a
 * plain xdg compositor), the agl_ handle stays null, WindowManager() returns
 * nullptr, and AglShell behaves exactly like XdgShell.
 */
class AglShell final : public XdgShell, public IShellWindowManager {
 public:
  AglShell() = default;

  [[nodiscard]] std::string_view Name() const override { return "agl"; }

  // Claims agl_shell; delegates every other global (xdg_wm_base, ...) to
  // XdgShell.
  [[nodiscard]] bool TryBindGlobal(wl_registry* registry,
                                   uint32_t name,
                                   std::string_view interface,
                                   uint32_t version) override;

  // Roundtrip for agl_shell.bound_ok / bound_fail (v2+). No-op if agl_shell was
  // not advertised.
  [[nodiscard]] bool Sync(wl_display* display) override;

  void OnClientReady() override;

  [[nodiscard]] IShellWindowManager* WindowManager() override {
    return agl_shell_ ? this : nullptr;
  }

  // --- IShellWindowManager (AGL-only WM ops) -------------------------------
  void SetBackground(wl_surface* surface, std::size_t output_index) override;
  void SetPanel(wl_surface* surface,
                SurfaceRole role,
                std::size_t output_index) override;
  void SetActivationArea(uint32_t x,
                         uint32_t y,
                         uint32_t width,
                         uint32_t height,
                         std::size_t output_index) override;

  /// The Display owns the wl_output list; it installs a resolver so the WM ops
  /// can map an output index to its wl_output*.
  using OutputResolver = std::function<wl_output*(std::size_t)>;
  void SetOutputResolver(OutputResolver resolver) override {
    output_resolver_ = std::move(resolver);
  }

  // --- wl::AglShellHandler<AglShell> callbacks -----------------------------
  void OnAglBoundOk() {
    bound_pending_ = false;
    bound_ok_ = true;
  }
  void OnAglBoundFail() {
    bound_pending_ = false;
    bound_ok_ = false;
  }
  void OnAglAppState(const char* /*app_id*/, uint32_t /*state*/) {}

 private:
  [[nodiscard]] wl_output* ResolveOutput(std::size_t index) const {
    return output_resolver_ ? output_resolver_(index) : nullptr;
  }

  wl::WlPtr<wl::AglShellHandler<AglShell>> agl_shell_;
  OutputResolver output_resolver_;
  uint32_t agl_version_{0};
  bool bound_pending_{false};
  bool bound_ok_{false};
};

}  // namespace ivi

#endif  // SHELL_WAYLAND_SHELL_AGL_SHELL_H_
