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

#include "wayland/shell/agl_shell.h"

#include <algorithm>

#include <wayland-client.h>

#include <wl/client_helpers.hpp>

#include "logging/logging.h"

namespace ivi {

namespace {
namespace ac = agl_shell::client;

uint32_t ToAglEdge(const SurfaceRole role) {
  switch (role) {
    case SurfaceRole::kPanelTop:
      return static_cast<uint32_t>(ac::AglShellEdge::Top);
    case SurfaceRole::kPanelBottom:
      return static_cast<uint32_t>(ac::AglShellEdge::Bottom);
    case SurfaceRole::kPanelLeft:
      return static_cast<uint32_t>(ac::AglShellEdge::Left);
    case SurfaceRole::kPanelRight:
      return static_cast<uint32_t>(ac::AglShellEdge::Right);
    default:
      return static_cast<uint32_t>(ac::AglShellEdge::Top);
  }
}
}  // namespace

bool AglShell::TryBindGlobal(wl_registry* registry,
                             const uint32_t name,
                             const std::string_view interface,
                             const uint32_t version) {
  if (interface == ac::agl_shell_traits::wl_iface().name) {
    const uint32_t bind_ver = std::min(version, ac::agl_shell_traits::version);
    auto* raw = wl_registry_bind(registry, name,
                                 &ac::agl_shell_traits::wl_iface(), bind_ver);
    if (!wl::SetupHandler(agl_shell_, reinterpret_cast<wl_proxy*>(raw))) {
      ihs::log::error("[AglShell] failed to set up agl_shell");
      return false;
    }
    agl_shell_.Get()->app_ = this;
    agl_version_ = bind_ver;
    ihs::log::debug("[AglShell] bound agl_shell v{}", bind_ver);
    return true;
  }
  // Everything else (xdg_wm_base, ...) is the standard xdg role.
  return XdgShell::TryBindGlobal(registry, name, interface, version);
}

bool AglShell::Sync(wl_display* display) {
  // bound_ok / bound_fail exist since agl_shell v2. Below that, or when
  // agl_shell was not advertised, there is nothing to wait for.
  if (!agl_shell_ || agl_version_ < 2) {
    return true;
  }
  // bound_ok/bound_fail may have already arrived during earlier display-init
  // roundtrips. Only block if we have not seen it yet, otherwise we would wait
  // for a second event that the compositor never sends.
  if (!bound_received_) {
    bound_pending_ = true;
    while (bound_pending_) {
      if (wl_display_roundtrip(display) < 0) {
        ihs::log::error(
            "[AglShell] roundtrip failed waiting for agl_shell.bound");
        return false;
      }
    }
  }
  if (!bound_ok_) {
    ihs::log::error("[AglShell] agl_shell already in use by another client");
  }
  return bound_ok_;
}

void AglShell::OnClientReady() {
  if (agl_shell_) {
    agl_shell_.Get()->Ready();
  }
}

void AglShell::SetBackground(wl_surface* surface,
                             const std::size_t output_index) {
  if (!agl_shell_) {
    return;
  }
  agl_shell_.Get()->SetBackground(
      reinterpret_cast<wl_proxy*>(surface),
      reinterpret_cast<wl_proxy*>(ResolveOutput(output_index)));
}

void AglShell::SetPanel(wl_surface* surface,
                        const SurfaceRole role,
                        const std::size_t output_index) {
  if (!agl_shell_) {
    return;
  }
  agl_shell_.Get()->SetPanel(
      reinterpret_cast<wl_proxy*>(surface),
      reinterpret_cast<wl_proxy*>(ResolveOutput(output_index)),
      ToAglEdge(role));
}

void AglShell::SetActivationArea(const uint32_t x,
                                 const uint32_t y,
                                 const uint32_t width,
                                 const uint32_t height,
                                 const std::size_t output_index) {
  if (!agl_shell_) {
    return;
  }
  ihs::log::debug("[AglShell] activation area [{}x{}+{}x{}]", width, height, x,
                  y);
  agl_shell_.Get()->SetActivateRegion(
      reinterpret_cast<wl_proxy*>(ResolveOutput(output_index)),
      static_cast<int32_t>(x), static_cast<int32_t>(y),
      static_cast<int32_t>(width), static_cast<int32_t>(height));
}

}  // namespace ivi
