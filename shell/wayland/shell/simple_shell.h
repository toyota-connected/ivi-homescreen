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

#ifndef SHELL_WAYLAND_SHELL_SIMPLE_SHELL_H_
#define SHELL_WAYLAND_SHELL_SIMPLE_SHELL_H_

#include <cstdint>
#include <vector>

#include <wayland-util.h>  // wl_fixed_t

#include "simple_shell_client.hpp"  // generated (simple_shell::client)

#include <wl/simple_shell.hpp>  // wl::SimpleShellHandler + interface tables
#include <wl/wl_ptr.hpp>

#include "wayland/shell/wayland_shell.h"

namespace ivi {

class SimpleShellSurface;

/**
 * @brief RDK/Westeros wl_simple_shell. Unlike xdg/ivi, a surface has no role
 * object: the compositor assigns each wl_surface a numeric id via the async
 * surface_id event, after which layout is driven imperatively
 * (set_geometry/set_visible/set_zorder) keyed by that id. SimpleShell tracks
 * its created surfaces and applies their layout once the id arrives.
 */
class SimpleShell final : public WaylandShell {
 public:
  SimpleShell() = default;

  [[nodiscard]] std::string_view Name() const override { return "simple"; }

  [[nodiscard]] bool TryBindGlobal(wl_registry* registry,
                                   uint32_t name,
                                   std::string_view interface,
                                   uint32_t version) override;

  [[nodiscard]] bool IsBound() const override {
    return static_cast<bool>(shell_);
  }

  [[nodiscard]] std::unique_ptr<ShellSurface> CreateSurface(
      wl_surface* surface,
      const WindowConfig& config) override;

  // SimpleShellSurface (de)registers so surface_id events can be routed to it.
  void Register(SimpleShellSurface* surface);
  void Unregister(SimpleShellSurface* surface);

  // --- wl::SimpleShellHandler<SimpleShell> callbacks -----------------------
  void OnSimpleShellSurfaceId(wl_proxy* surface, uint32_t surface_id);
  void OnSimpleShellSurfaceCreated(uint32_t /*id*/, const char* /*name*/) {}
  void OnSimpleShellSurfaceDestroyed(uint32_t /*id*/, const char* /*name*/) {}
  void OnSimpleShellSurfaceStatus(uint32_t /*id*/,
                                  const char* /*name*/,
                                  uint32_t /*visible*/,
                                  int32_t /*x*/,
                                  int32_t /*y*/,
                                  int32_t /*width*/,
                                  int32_t /*height*/,
                                  wl_fixed_t /*opacity*/,
                                  wl_fixed_t /*zorder*/) {}
  void OnSimpleShellGetSurfacesDone() {}
  void OnSimpleShellPopupDetails(uint32_t /*id*/,
                                 uint32_t /*parent_id*/,
                                 int32_t /*popup*/) {}

 private:
  wl::WlPtr<wl::SimpleShellHandler<SimpleShell>> shell_;
  std::vector<SimpleShellSurface*> surfaces_;  // non-owning; owned by windows
};

}  // namespace ivi

#endif  // SHELL_WAYLAND_SHELL_SIMPLE_SHELL_H_
