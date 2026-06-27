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

#include "wayland/shell/simple_shell.h"

#include <algorithm>
#include <utility>

#include <wayland-client.h>

#include <wl/client_helpers.hpp>  // wl::SetupHandler

#include "logging/logging.h"

namespace ivi {

namespace {
namespace sc = simple_shell::client;
}  // namespace

/**
 * @brief One wl_simple_shell surface. It has no protocol role object; it holds
 * its base wl_surface and the desired geometry, and applies them once the
 * compositor delivers its surface id (SimpleShell::OnSimpleShellSurfaceId).
 */
class SimpleShellSurface final : public ShellSurface {
 public:
  SimpleShellSurface(SimpleShell& shell,
                     wl_surface* surface,
                     WindowConfig config)
      : shell_(shell), surface_(surface), config_(std::move(config)) {
    shell_.Register(this);
  }

  ~SimpleShellSurface() override { shell_.Unregister(this); }

  [[nodiscard]] wl_surface* surface() const { return surface_; }
  [[nodiscard]] const WindowConfig& config() const { return config_; }
  [[nodiscard]] uint32_t id() const { return id_; }
  void set_id(const uint32_t id) { id_ = id; }

 private:
  SimpleShell& shell_;
  wl_surface* surface_;
  WindowConfig config_;
  uint32_t id_{0};
};

bool SimpleShell::TryBindGlobal(wl_registry* registry,
                                const uint32_t name,
                                const std::string_view interface,
                                const uint32_t version) {
  if (interface != sc::wl_simple_shell_traits::wl_iface().name) {
    return false;
  }
  const uint32_t bind_ver =
      std::min(version, sc::wl_simple_shell_traits::version);
  auto* raw = wl_registry_bind(
      registry, name, &sc::wl_simple_shell_traits::wl_iface(), bind_ver);
  if (!wl::SetupHandler(shell_, reinterpret_cast<wl_proxy*>(raw))) {
    spdlog::error("[SimpleShell] failed to set up wl_simple_shell");
    return false;
  }
  shell_.Get()->app_ = this;
  spdlog::debug("[SimpleShell] bound wl_simple_shell v{}", bind_ver);
  return true;
}

std::unique_ptr<ShellSurface> SimpleShell::CreateSurface(
    wl_surface* surface,
    const WindowConfig& config) {
  if (!shell_) {
    spdlog::error("[SimpleShell] CreateSurface before wl_simple_shell bound");
    return nullptr;
  }
  return std::make_unique<SimpleShellSurface>(*this, surface, config);
}

void SimpleShell::Register(SimpleShellSurface* surface) {
  surfaces_.push_back(surface);
}

void SimpleShell::Unregister(SimpleShellSurface* surface) {
  surfaces_.erase(std::remove(surfaces_.begin(), surfaces_.end(), surface),
                  surfaces_.end());
}

void SimpleShell::OnSimpleShellSurfaceId(wl_proxy* surface,
                                         const uint32_t surface_id) {
  const auto it = std::find_if(
      surfaces_.begin(), surfaces_.end(), [surface](SimpleShellSurface* s) {
        return reinterpret_cast<wl_proxy*>(s->surface()) == surface;
      });
  if (it == surfaces_.end()) {
    spdlog::debug("[SimpleShell] surface_id {} for unknown surface", surface_id);
    return;
  }
  SimpleShellSurface* s = *it;
  s->set_id(surface_id);

  auto* shell = shell_.Get();
  const auto& cfg = s->config();
  shell->SetName(surface_id, cfg.app_id.c_str());
  shell->SetGeometry(surface_id, /*x=*/0, /*y=*/0, cfg.width, cfg.height);
  shell->SetZorder(surface_id, wl_fixed_from_double(0.5));
  shell->SetOpacity(surface_id, wl_fixed_from_double(1.0));
  shell->SetVisible(surface_id, /*visible=*/1);
  spdlog::debug("[SimpleShell] surface_id {} laid out {}x{}", surface_id,
                cfg.width, cfg.height);
}

}  // namespace ivi
