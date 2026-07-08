/*
 * Copyright 2026 Toyota Connected North America
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

#ifndef SHELL_ACCESSIBILITY_SEMANTICS_TRANSLATOR_H_
#define SHELL_ACCESSIBILITY_SEMANTICS_TRANSLATOR_H_

#include <cstdint>

#include <shell/platform/embedder/embedder.h>

namespace accessibility {

// Backend-neutral semantic role. Deliberately independent of accesskit_role
// (a uint8_t enum whose numeric values have shifted across releases): the
// AccessKit bridge maps this to accesskit_role at the boundary, which keeps
// this translator free of any accesskit.h dependency and therefore unit-
// testable in every build configuration.
enum class Role : uint8_t {
  kUnknown,
  kWindow,
  kButton,
  kTextInput,
  kMultilineTextInput,
  kPasswordInput,
  kSlider,
  kSwitch,
  kCheckBox,
  kRadioButton,
  kLink,
  kImage,
  kHeading,
  kScrollView,
  kPane,
  kLabel,
  kGenericContainer,
};

// The derived, backend-neutral semantic attributes of a node: the pure mapping
// of a Flutter semantics node's identity, flags, and action bitmask. Text,
// bounds, and transform stay on the mirror (direct copies, not derived), so
// this POD carries only what the translation logic computes.
struct NodeSpec {
  Role role = Role::kUnknown;

  // States derived from flags.
  bool disabled = false;       // HasEnabledState && !IsEnabled
  bool hidden = false;         // IsHidden
  bool read_only = false;      // IsReadOnly
  bool selected = false;       // IsSelected
  bool expanded = false;       // HasExpandedState && IsExpanded
  bool checked = false;        // HasCheckedState && IsChecked
  bool checked_mixed = false;  // IsCheckStateMixed
  bool toggled = false;        // HasToggledState && IsToggled
  bool live = false;           // IsLiveRegion
  bool focusable = false;      // IsFocusable
  bool focused = false;        // IsFocused

  // Actions the AT can invoke, from the action bitmask.
  bool action_tap = false;
  bool action_long_press = false;
  bool action_increase = false;
  bool action_decrease = false;
  bool action_scroll_up = false;
  bool action_scroll_down = false;
  bool action_scroll_left = false;
  bool action_scroll_right = false;
  bool action_show_on_screen = false;
  bool action_set_text = false;
  bool action_focus = false;
};

// Pure translation from a Flutter semantics node's identity + flags + actions
// to a NodeSpec. `has_label` / `has_children` disambiguate a label-only leaf
// from a generic container when no stronger role flag matches. Stateless and
// side-effect free.
NodeSpec Translate(int32_t id,
                   FlutterSemanticsFlag flags,
                   FlutterSemanticsAction actions,
                   bool has_label,
                   bool has_children);

}  // namespace accessibility

#endif  // SHELL_ACCESSIBILITY_SEMANTICS_TRANSLATOR_H_
