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

#include "wayland/shell/wayland_shell.h"

#if ENABLE_XDG_CLIENT || ENABLE_AGL_SHELL_CLIENT
#include "wayland/shell/xdg_shell.h"
#endif
#if ENABLE_AGL_SHELL_CLIENT
#include "wayland/shell/agl_shell.h"
#endif
#if ENABLE_IVI_SHELL_CLIENT
#include "wayland/shell/ivi_shell.h"
#endif
#if ENABLE_SIMPLE_SHELL_CLIENT
#include "wayland/shell/simple_shell.h"
#endif

#include "logging/logging.h"

namespace ivi {

std::vector<std::unique_ptr<WaylandShell>> WaylandShell::Create(
    const std::string_view selection,
    const bool wants_agl_extension) {
  const std::string_view sel = selection.empty() ? "auto" : selection;
  const bool is_auto = (sel == "auto");
  // Only referenced inside the ivi / simple opt-in blocks below, both of which
  // are #if-gated off by default — mark maybe_unused so the default build (xdg
  // family only) doesn't warn.
  [[maybe_unused]] auto want = [&](const std::string_view name) {
    return is_auto || sel == name;
  };

  std::vector<std::unique_ptr<WaylandShell>> shells;

  // ivi / simple are explicit opt-ins (off by default). In "auto" they take
  // priority over the xdg family so that enabling them actually selects them on
  // a compositor that advertises both (e.g. weston-ivi also exports xdg).
#if ENABLE_IVI_SHELL_CLIENT
  if (want("ivi")) {
    shells.push_back(std::make_unique<IviShell>());
  }
#endif
#if ENABLE_SIMPLE_SHELL_CLIENT
  if (want("simple")) {
    shells.push_back(std::make_unique<SimpleShell>());
  }
#endif

  // xdg family. AglShell covers xdg + the agl extension and degrades to plain
  // xdg when agl_shell is absent, so it serves both "auto" and "agl". Explicit
  // "xdg" forces the plain xdg shell (no agl extension).
#if ENABLE_AGL_SHELL_CLIENT
  // Under "auto", take the agl extension only when a view actually needs it.
  //
  // AglShell degrades to plain xdg when agl_shell is absent, so selecting it
  // unconditionally looks harmless and is not: a compositor admits one
  // agl_shell client at a time. On a system where something else owns the
  // screen, an ordinary application binding it leaves that one unable to.
  // Before the shell layer was unified this was gated on the same question --
  // Display only set bind_to_agl_shell when some view's window_type was not
  // NORMAL -- and losing that gate is what put every "auto" client on
  // agl_shell. Explicit "agl" still forces it.
  if (sel == "agl" || (is_auto && wants_agl_extension)) {
    shells.push_back(std::make_unique<AglShell>());
  } else if (is_auto || sel == "xdg") {
    shells.push_back(std::make_unique<XdgShell>());
  }
#elif ENABLE_XDG_CLIENT
  if (want("xdg")) {
    shells.push_back(std::make_unique<XdgShell>());
  }
#endif

  if (shells.empty()) {
    ihs::log::warn("[WaylandShell] no shell matched selection '{}'", sel);
  }
  return shells;
}

}  // namespace ivi
