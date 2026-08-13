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

// A property that may not apply to a node at all. The engine's newer flags
// struct draws this distinction and the old enum could not: a node for which
// "expanded" is meaningless is not the same as one that is collapsed, and
// answering both with false loses which is which.
enum class Tristate : uint8_t {
  kNone,  // does not apply to this node
  kTrue,
  kFalse,
};

// Check state, which unlike the other tristates has a fourth value.
enum class CheckState : uint8_t {
  kNone,  // not a checkable node
  kTrue,
  kFalse,
  kMixed,
};

// The derived, backend-neutral semantic attributes of a node: the pure mapping
// of a Flutter semantics node's identity, flags, and action bitmask. Text,
// bounds, and transform stay on the mirror (direct copies, not derived), so
// this POD carries only what the translation logic computes.
struct NodeSpec {
  Role role = Role::kUnknown;

  // States derived from flags.
  bool disabled = false;       // enabled state present and not enabled
  bool hidden = false;         // IsHidden
  bool read_only = false;      // IsReadOnly
  bool selected = false;       // IsSelected
  bool expanded = false;       // expanded state present and expanded
  bool checked = false;        // checked state present and checked
  bool checked_mixed = false;  // checked state is mixed (tristate checkbox)
  bool toggled = false;        // toggled state present and on
  bool live = false;           // IsLiveRegion
  bool focusable = false;      // IsFocusable
  bool focused = false;        // IsFocused

  // States only the FlutterSemanticsFlags struct can express. Always false
  // when translated from the legacy enum, which has no equivalent bit.
  bool required = false;            // input required before form submission
  bool a11y_focus_blocked = false;  // node blocks accessibility focus

  // Three-valued forms of the states above. The bools are these collapsed for
  // the callers that only ever ask "is it on", and are lossy by construction:
  // `disabled` cannot distinguish an enabled node from one where enablement is
  // meaningless, because both are false. Anything publishing state onward
  // should carry these instead.
  CheckState check_state = CheckState::kNone;
  Tristate enabled_state = Tristate::kNone;
  Tristate selected_state = Tristate::kNone;
  Tristate toggled_state = Tristate::kNone;
  Tristate expanded_state = Tristate::kNone;
  Tristate focused_state = Tristate::kNone;
  Tristate required_state = Tristate::kNone;

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
  bool action_scroll_to_offset = false;
  bool action_expand = false;
  bool action_collapse = false;
  // Set when the node declares application-defined actions. The ids
  // themselves travel separately; this is what marks the node as able to
  // run one at all.
  bool action_custom_action = false;
};

// Pure translation from a Flutter semantics node's identity + flags + actions
// to a NodeSpec. `has_label` / `has_children` disambiguate a label-only leaf
// from a generic container when no stronger role flag matches. Stateless and
// side-effect free.
//
// Preferred form. `flags` is `FlutterSemanticsNode2::flags2`, which the engine
// may leave null; when it is null the translation falls back to `legacy_flags`
// (`flags__deprecated__`). The struct is the richer source — its tristates
// distinguish "property does not apply" from "applies and is false", which the
// enum could only encode as a pair of bits — so it wins whenever present.
NodeSpec Translate(int32_t id,
                   const FlutterSemanticsFlags* flags,
                   FlutterSemanticsFlag legacy_flags,
                   FlutterSemanticsAction actions,
                   bool has_label,
                   bool has_children);

// Legacy-enum-only overload, equivalent to passing a null `flags`. Retained
// for callers that have no FlutterSemanticsFlags to hand.
NodeSpec Translate(int32_t id,
                   FlutterSemanticsFlag flags,
                   FlutterSemanticsAction actions,
                   bool has_label,
                   bool has_children);

}  // namespace accessibility

#endif  // SHELL_ACCESSIBILITY_SEMANTICS_TRANSLATOR_H_
