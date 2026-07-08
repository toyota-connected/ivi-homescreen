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

#include "semantics_translator.h"

namespace accessibility {

namespace {

bool Has(FlutterSemanticsFlag flags, FlutterSemanticsFlag bit) {
  return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0;
}

bool Has(FlutterSemanticsAction actions, FlutterSemanticsAction bit) {
  return (static_cast<uint32_t>(actions) & static_cast<uint32_t>(bit)) != 0;
}

// Role, first-match-wins. The order encodes precedence: a text field that is
// also focusable is a text input, not a generic container; a checkbox in a
// mutually-exclusive group is a radio button; etc.
Role DeriveRole(int32_t id,
                FlutterSemanticsFlag flags,
                bool has_label,
                bool has_children) {
  if (id == 0) {
    return Role::kWindow;
  }
  if (Has(flags, kFlutterSemanticsFlagIsTextField)) {
    if (Has(flags, kFlutterSemanticsFlagIsObscured)) {
      return Role::kPasswordInput;
    }
    if (Has(flags, kFlutterSemanticsFlagIsMultiline)) {
      return Role::kMultilineTextInput;
    }
    return Role::kTextInput;
  }
  if (Has(flags, kFlutterSemanticsFlagIsSlider)) {
    return Role::kSlider;
  }
  if (Has(flags, kFlutterSemanticsFlagHasToggledState)) {
    return Role::kSwitch;
  }
  if (Has(flags, kFlutterSemanticsFlagHasCheckedState)) {
    return Has(flags, kFlutterSemanticsFlagIsInMutuallyExclusiveGroup)
               ? Role::kRadioButton
               : Role::kCheckBox;
  }
  if (Has(flags, kFlutterSemanticsFlagIsButton)) {
    return Role::kButton;
  }
  if (Has(flags, kFlutterSemanticsFlagIsLink)) {
    return Role::kLink;
  }
  if (Has(flags, kFlutterSemanticsFlagIsImage)) {
    return Role::kImage;
  }
  if (Has(flags, kFlutterSemanticsFlagIsHeader)) {
    return Role::kHeading;
  }
  if (Has(flags, kFlutterSemanticsFlagIsKeyboardKey)) {
    return Role::kButton;  // documented approximation: no AT-SPI keyboard-key
  }
  if (Has(flags, kFlutterSemanticsFlagHasImplicitScrolling)) {
    return Role::kScrollView;
  }
  if (Has(flags, kFlutterSemanticsFlagScopesRoute) ||
      Has(flags, kFlutterSemanticsFlagNamesRoute)) {
    return Role::kPane;
  }
  if (has_label && !has_children) {
    return Role::kLabel;
  }
  return Role::kGenericContainer;
}

}  // namespace

NodeSpec Translate(int32_t id,
                   FlutterSemanticsFlag flags,
                   FlutterSemanticsAction actions,
                   bool has_label,
                   bool has_children) {
  NodeSpec spec;
  spec.role = DeriveRole(id, flags, has_label, has_children);

  spec.disabled = Has(flags, kFlutterSemanticsFlagHasEnabledState) &&
                  !Has(flags, kFlutterSemanticsFlagIsEnabled);
  spec.hidden = Has(flags, kFlutterSemanticsFlagIsHidden);
  spec.read_only = Has(flags, kFlutterSemanticsFlagIsReadOnly);
  spec.selected = Has(flags, kFlutterSemanticsFlagIsSelected);
  spec.expanded = Has(flags, kFlutterSemanticsFlagHasExpandedState) &&
                  Has(flags, kFlutterSemanticsFlagIsExpanded);
  spec.checked = Has(flags, kFlutterSemanticsFlagHasCheckedState) &&
                 Has(flags, kFlutterSemanticsFlagIsChecked);
  spec.checked_mixed = Has(flags, kFlutterSemanticsFlagIsCheckStateMixed);
  spec.toggled = Has(flags, kFlutterSemanticsFlagHasToggledState) &&
                 Has(flags, kFlutterSemanticsFlagIsToggled);
  spec.live = Has(flags, kFlutterSemanticsFlagIsLiveRegion);
  spec.focusable = Has(flags, kFlutterSemanticsFlagIsFocusable);
  spec.focused = Has(flags, kFlutterSemanticsFlagIsFocused);

  spec.action_tap = Has(actions, kFlutterSemanticsActionTap);
  spec.action_long_press = Has(actions, kFlutterSemanticsActionLongPress);
  spec.action_increase = Has(actions, kFlutterSemanticsActionIncrease);
  spec.action_decrease = Has(actions, kFlutterSemanticsActionDecrease);
  spec.action_scroll_up = Has(actions, kFlutterSemanticsActionScrollUp);
  spec.action_scroll_down = Has(actions, kFlutterSemanticsActionScrollDown);
  spec.action_scroll_left = Has(actions, kFlutterSemanticsActionScrollLeft);
  spec.action_scroll_right = Has(actions, kFlutterSemanticsActionScrollRight);
  spec.action_show_on_screen =
      Has(actions, kFlutterSemanticsActionShowOnScreen);
  spec.action_set_text = Has(actions, kFlutterSemanticsActionSetText);
  spec.action_focus = Has(actions, kFlutterSemanticsActionFocus);

  return spec;
}

}  // namespace accessibility
