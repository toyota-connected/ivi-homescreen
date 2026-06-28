// Copyright 2020-2026 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string_view>

namespace homescreen {

// Maps a Flutter SystemMouseCursors `kind` (the string sent over the
// flutter/mousecursor platform channel) to a freedesktop XCursor name that
// installed XCursor themes provide. The backend cursor implementations
// (Wayland wl_cursor theme, DRM drm::cursor::Theme, software drm::cursor) all
// resolve the returned name against a theme.
//
// Returns nullptr for "none" — the caller should hide the cursor — and
// "default" for any unrecognized kind (themes always ship "default").
[[nodiscard]] inline const char* CursorKindToXcursorName(
    const std::string_view kind) {
  if (kind == "none") {
    return nullptr;
  }

  struct Entry {
    std::string_view kind;
    const char* name;
  };
  // Flutter kind -> XCursor/CSS cursor name. Ordered roughly as in Flutter's
  // SystemMouseCursors. Names follow the CSS cursor spec, which modern XCursor
  // themes (Adwaita, breeze, …) ship; older themes fall back via theme
  // inheritance to the legacy aliases (e.g. ew-resize -> sb_h_double_arrow).
  static constexpr Entry kMap[] = {
      {"basic", "default"},
      {"click", "pointer"},
      {"forbidden", "not-allowed"},
      {"wait", "wait"},
      {"progress", "progress"},
      {"contextMenu", "context-menu"},
      {"help", "help"},
      {"text", "text"},
      {"verticalText", "vertical-text"},
      {"cell", "cell"},
      {"precise", "crosshair"},
      {"move", "move"},
      {"grab", "grab"},
      {"grabbing", "grabbing"},
      {"noDrop", "no-drop"},
      {"alias", "alias"},
      {"copy", "copy"},
      {"allScroll", "all-scroll"},
      {"resizeLeftRight", "ew-resize"},
      {"resizeUpDown", "ns-resize"},
      {"resizeUpLeftDownRight", "nwse-resize"},
      {"resizeUpRightDownLeft", "nesw-resize"},
      {"resizeUp", "n-resize"},
      {"resizeDown", "s-resize"},
      {"resizeLeft", "w-resize"},
      {"resizeRight", "e-resize"},
      {"resizeUpLeft", "nw-resize"},
      {"resizeUpRight", "ne-resize"},
      {"resizeDownLeft", "sw-resize"},
      {"resizeDownRight", "se-resize"},
      {"resizeColumn", "col-resize"},
      {"resizeRow", "row-resize"},
      {"zoomIn", "zoom-in"},
      {"zoomOut", "zoom-out"},
  };
  for (const auto& e : kMap) {
    if (e.kind == kind) {
      return e.name;
    }
  }
  return "default";
}

}  // namespace homescreen
