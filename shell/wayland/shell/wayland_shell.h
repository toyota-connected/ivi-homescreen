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

#ifndef SHELL_WAYLAND_SHELL_WAYLAND_SHELL_H_
#define SHELL_WAYLAND_SHELL_WAYLAND_SHELL_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct wl_registry;
struct wl_surface;
struct wl_output;
struct wl_display;

namespace ivi {

/**
 * @brief The surface "role" the compositor shell assigns to a top-level
 * window. Mirrors WaylandWindow::window_type but kept independent so the shell
 * layer does not depend on window.h (avoids an include cycle: window.cc owns a
 * ShellSurface).
 */
enum class SurfaceRole {
  kNormal,
  kBackground,
  kPanelTop,
  kPanelBottom,
  kPanelLeft,
  kPanelRight,
};

/**
 * @brief Everything a shell needs to assign a role to a base wl_surface and to
 * report compositor-driven geometry back to the owning WaylandWindow.
 *
 * The on_configure / on_close callbacks replace the per-protocol static
 * listener functions that used to live on WaylandWindow (handle_xdg_surface_
 * configure, handle_toplevel_configure, handle_ivi_surface_configure, ...).
 * Each ShellSurface installs its protocol's listener internally and forwards
 * normalized events through these.
 */
/**
 * @brief Normalized compositor configure event delivered to the window. Carries
 * the requested size plus the xdg_toplevel.configure state flags (parsed from
 * the protocol's `states` array). Shells without window states (ivi, simple)
 * report all flags false.
 */
struct SurfaceConfigure {
  int32_t width{};
  int32_t height{};
  bool fullscreen{};
  bool maximized{};
  bool resizing{};
  bool activated{};
};

struct WindowConfig {
  SurfaceRole role{SurfaceRole::kNormal};
  std::string app_id;
  wl_output* output{};  // target output (fullscreen / background / panel)
  std::size_t output_index{};
  bool fullscreen{};
  int32_t width{};
  int32_t height{};
  uint32_t ivi_surface_id{};  // ivi-shell only; 0 = auto/unused

  // Compositor reported the surface size + window state (xdg_toplevel /
  // ivi_surface configure).
  std::function<void(const SurfaceConfigure&)> on_configure;
  // Compositor asked the toplevel to close (xdg_toplevel.close). May be unset.
  std::function<void()> on_close;
};

/**
 * @brief Per-window surface role produced by a WaylandShell.
 *
 * Owns the protocol objects that give a wl_surface its role (xdg_surface +
 * xdg_toplevel, or ivi_surface, or a simple_shell surface id) and tears them
 * down in the destructor. Replaces the ivi-vs-xdg branch and the role-specific
 * members/listeners that used to live directly on WaylandWindow.
 */
class ShellSurface {
 public:
  virtual ~ShellSurface() = default;

  ShellSurface(const ShellSurface&) = delete;
  ShellSurface& operator=(const ShellSurface&) = delete;

  /// Update the human-readable title (xdg_toplevel.set_title). No-op where the
  /// shell has no title concept (ivi, simple_shell).
  virtual void SetTitle(const std::string& /*title*/) {}

  /// Request fullscreen on the given output (xdg_toplevel.set_fullscreen).
  virtual void SetFullscreen(wl_output* /*output*/) {}

 protected:
  ShellSurface() = default;
};

/**
 * @brief A compositor shell binding (xdg / agl / ivi / simple_shell).
 *
 * One implementation per protocol family. The Display owns a priority-ordered
 * vector built by WaylandShell::Create() and, during registry enumeration,
 * offers each unclaimed global to every shell via TryBindGlobal(). The first
 * shell that both is selected (--shell) and binds its mandatory global becomes
 * the active shell that assigns surface roles.
 */
class WaylandShell {
 public:
  virtual ~WaylandShell() = default;

  WaylandShell(const WaylandShell&) = delete;
  WaylandShell& operator=(const WaylandShell&) = delete;

  /// Stable identifier: "xdg" | "agl" | "ivi" | "simple". Matched against the
  /// --shell selection.
  [[nodiscard]] virtual std::string_view Name() const = 0;

  /// Offered each registry global the core did not consume. Bind and return
  /// true if @p interface belongs to this shell; return false otherwise.
  [[nodiscard]] virtual bool TryBindGlobal(wl_registry* registry,
                                           uint32_t name,
                                           std::string_view interface,
                                           uint32_t version) = 0;

  /// True once this shell has bound its mandatory global(s) and can assign
  /// roles.
  [[nodiscard]] virtual bool IsBound() const = 0;

  /// Post-registry handshake. AGL overrides this to roundtrip for
  /// agl_shell.bound_ok/bound_fail before any surface is created; the default
  /// is a no-op. Returns false if the handshake failed (e.g. shell already
  /// owned by another client).
  [[nodiscard]] virtual bool Sync(wl_display* /*display*/) { return true; }

  /// Give @p surface its role and return an owning ShellSurface. Replaces the
  /// xdg-vs-ivi branch in WaylandWindow::WaylandWindow.
  [[nodiscard]] virtual std::unique_ptr<ShellSurface> CreateSurface(
      wl_surface* surface,
      const WindowConfig& config) = 0;

  /// Signal the compositor the client is ready to be shown. AGL maps this to
  /// agl_shell.ready(); others no-op.
  virtual void OnClientReady() {}

  /// Install a resolver mapping an output index to its wl_output*. Only AGL's
  /// window-management facet needs it (set_background/set_panel take an
  /// output); the Display sets it after registry enumeration. Default no-op.
  virtual void SetOutputResolver(std::function<wl_output*(std::size_t)>) {}

  /// Optional AGL-style window-management facet (set_background / set_panel /
  /// activation region / app stack). Returns nullptr for shells without one;
  /// only AglShell returns non-null. Callers use this instead of dynamic_cast.
  [[nodiscard]] virtual class IShellWindowManager* WindowManager() {
    return nullptr;
  }

  /**
   * @brief Build the priority-ordered set of shells the build enables,
   * honoring the @p selection ("auto" | "xdg" | "agl" | "ivi" | "simple").
   * "auto" returns all enabled shells in priority order so the first to bind
   * wins; a named value returns just that shell. The Display picks the active
   * shell as the first one whose IsBound() is true after registry enumeration.
   */
  [[nodiscard]] static std::vector<std::unique_ptr<WaylandShell>> Create(
      std::string_view selection);

 protected:
  WaylandShell() = default;
};

/**
 * @brief AGL-specific window management (no xdg/ivi analog). Implemented only
 * by AglShell; reached via WaylandShell::WindowManager() so app.cc no longer
 * dynamic_casts the Display to drive agl_shell.
 */
class IShellWindowManager {
 public:
  virtual ~IShellWindowManager() = default;

  virtual void SetBackground(wl_surface* surface, std::size_t output_index) = 0;
  virtual void SetPanel(wl_surface* surface,
                        SurfaceRole role,
                        std::size_t output_index) = 0;
  virtual void SetActivationArea(uint32_t x,
                                 uint32_t y,
                                 uint32_t width,
                                 uint32_t height,
                                 std::size_t output_index) = 0;
};

}  // namespace ivi

#endif  // SHELL_WAYLAND_SHELL_WAYLAND_SHELL_H_
