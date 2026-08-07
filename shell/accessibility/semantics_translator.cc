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

// The two flag representations normalized to one shape, so the role table and
// the state table below are written once rather than once per source.
//
// The struct form carries tristates where the enum needed a pair of bits
// ("has X state" + "is X"); both collapse to the same present/value pair here.
struct FlagView {
  bool is_text_field = false;
  bool is_obscured = false;
  bool is_multiline = false;
  bool is_slider = false;
  bool is_button = false;
  bool is_link = false;
  bool is_image = false;
  bool is_header = false;
  bool is_keyboard_key = false;
  bool is_in_mutually_exclusive_group = false;
  bool has_implicit_scrolling = false;
  bool scopes_route = false;
  bool names_route = false;

  bool has_enabled_state = false;
  bool is_enabled = false;
  bool has_checked_state = false;
  bool is_checked = false;
  bool is_check_state_mixed = false;
  bool has_toggled_state = false;
  bool is_toggled = false;
  bool has_expanded_state = false;
  bool is_expanded = false;

  bool is_hidden = false;
  bool is_read_only = false;
  bool is_selected = false;
  bool is_live_region = false;
  bool is_focusable = false;
  bool is_focused = false;

  // No legacy enum bit exists for these; they stay false when translated from
  // the enum rather than being guessed at.
  bool is_required = false;
  bool is_accessibility_focus_blocked = false;
};

FlagView FromLegacy(FlutterSemanticsFlag flags) {
  FlagView v;
  v.is_text_field = Has(flags, kFlutterSemanticsFlagIsTextField);
  v.is_obscured = Has(flags, kFlutterSemanticsFlagIsObscured);
  v.is_multiline = Has(flags, kFlutterSemanticsFlagIsMultiline);
  v.is_slider = Has(flags, kFlutterSemanticsFlagIsSlider);
  v.is_button = Has(flags, kFlutterSemanticsFlagIsButton);
  v.is_link = Has(flags, kFlutterSemanticsFlagIsLink);
  v.is_image = Has(flags, kFlutterSemanticsFlagIsImage);
  v.is_header = Has(flags, kFlutterSemanticsFlagIsHeader);
  v.is_keyboard_key = Has(flags, kFlutterSemanticsFlagIsKeyboardKey);
  v.is_in_mutually_exclusive_group =
      Has(flags, kFlutterSemanticsFlagIsInMutuallyExclusiveGroup);
  v.has_implicit_scrolling =
      Has(flags, kFlutterSemanticsFlagHasImplicitScrolling);
  v.scopes_route = Has(flags, kFlutterSemanticsFlagScopesRoute);
  v.names_route = Has(flags, kFlutterSemanticsFlagNamesRoute);

  v.has_enabled_state = Has(flags, kFlutterSemanticsFlagHasEnabledState);
  v.is_enabled = Has(flags, kFlutterSemanticsFlagIsEnabled);
  v.has_checked_state = Has(flags, kFlutterSemanticsFlagHasCheckedState);
  v.is_checked = Has(flags, kFlutterSemanticsFlagIsChecked);
  v.is_check_state_mixed = Has(flags, kFlutterSemanticsFlagIsCheckStateMixed);
  v.has_toggled_state = Has(flags, kFlutterSemanticsFlagHasToggledState);
  v.is_toggled = Has(flags, kFlutterSemanticsFlagIsToggled);
  v.has_expanded_state = Has(flags, kFlutterSemanticsFlagHasExpandedState);
  v.is_expanded = Has(flags, kFlutterSemanticsFlagIsExpanded);

  v.is_hidden = Has(flags, kFlutterSemanticsFlagIsHidden);
  v.is_read_only = Has(flags, kFlutterSemanticsFlagIsReadOnly);
  v.is_selected = Has(flags, kFlutterSemanticsFlagIsSelected);
  v.is_live_region = Has(flags, kFlutterSemanticsFlagIsLiveRegion);
  v.is_focusable = Has(flags, kFlutterSemanticsFlagIsFocusable);
  v.is_focused = Has(flags, kFlutterSemanticsFlagIsFocused);
  return v;
}

FlagView FromStruct(const FlutterSemanticsFlags& flags) {
  FlagView v;
  v.is_text_field = flags.is_text_field;
  v.is_obscured = flags.is_obscured;
  v.is_multiline = flags.is_multiline;
  v.is_slider = flags.is_slider;
  v.is_button = flags.is_button;
  v.is_link = flags.is_link;
  v.is_image = flags.is_image;
  v.is_header = flags.is_header;
  v.is_keyboard_key = flags.is_keyboard_key;
  v.is_in_mutually_exclusive_group = flags.is_in_mutually_exclusive_group;
  v.has_implicit_scrolling = flags.has_implicit_scrolling;
  v.scopes_route = flags.scopes_route;
  v.names_route = flags.names_route;

  // Tristate/check-state to present+value. kFlutterTristateNone and
  // kFlutterCheckStateNone mean the property does not apply to this node,
  // which is what the legacy enum spelled as "has X state" being unset.
  v.has_enabled_state = flags.is_enabled != kFlutterTristateNone;
  v.is_enabled = flags.is_enabled == kFlutterTristateTrue;
  v.has_checked_state = flags.is_checked != kFlutterCheckStateNone;
  v.is_checked = flags.is_checked == kFlutterCheckStateTrue;
  v.is_check_state_mixed = flags.is_checked == kFlutterCheckStateMixed;
  v.has_toggled_state = flags.is_toggled != kFlutterTristateNone;
  v.is_toggled = flags.is_toggled == kFlutterTristateTrue;
  v.has_expanded_state = flags.is_expanded != kFlutterTristateNone;
  v.is_expanded = flags.is_expanded == kFlutterTristateTrue;

  v.is_hidden = flags.is_hidden;
  v.is_read_only = flags.is_read_only;
  v.is_selected = flags.is_selected == kFlutterTristateTrue;
  v.is_live_region = flags.is_live_region;
  // The struct has no is_focusable; a node the engine reports a focus state
  // for is focusable by construction.
  v.is_focusable = flags.is_focused != kFlutterTristateNone;
  v.is_focused = flags.is_focused == kFlutterTristateTrue;

  v.is_required = flags.is_required == kFlutterTristateTrue;
  v.is_accessibility_focus_blocked = flags.is_accessibility_focus_blocked;
  return v;
}

// Role, first-match-wins. The order encodes precedence: a text field that is
// also focusable is a text input, not a generic container; a checkbox in a
// mutually-exclusive group is a radio button; etc.
Role DeriveRole(int32_t id,
                const FlagView& flags,
                bool has_label,
                bool has_children) {
  if (id == 0) {
    return Role::kWindow;
  }
  if (flags.is_text_field) {
    if (flags.is_obscured) {
      return Role::kPasswordInput;
    }
    if (flags.is_multiline) {
      return Role::kMultilineTextInput;
    }
    return Role::kTextInput;
  }
  if (flags.is_slider) {
    return Role::kSlider;
  }
  if (flags.has_toggled_state) {
    return Role::kSwitch;
  }
  if (flags.has_checked_state) {
    return flags.is_in_mutually_exclusive_group ? Role::kRadioButton
                                                : Role::kCheckBox;
  }
  if (flags.is_button) {
    return Role::kButton;
  }
  if (flags.is_link) {
    return Role::kLink;
  }
  if (flags.is_image) {
    return Role::kImage;
  }
  if (flags.is_header) {
    return Role::kHeading;
  }
  if (flags.is_keyboard_key) {
    return Role::kButton;  // documented approximation: no AT-SPI keyboard-key
  }
  if (flags.has_implicit_scrolling) {
    return Role::kScrollView;
  }
  if (flags.scopes_route || flags.names_route) {
    return Role::kPane;
  }
  if (has_label && !has_children) {
    return Role::kLabel;
  }
  return Role::kGenericContainer;
}

NodeSpec TranslateView(int32_t id,
                       const FlagView& flags,
                       FlutterSemanticsAction actions,
                       bool has_label,
                       bool has_children) {
  NodeSpec spec;
  spec.role = DeriveRole(id, flags, has_label, has_children);

  spec.disabled = flags.has_enabled_state && !flags.is_enabled;
  spec.hidden = flags.is_hidden;
  spec.read_only = flags.is_read_only;
  spec.selected = flags.is_selected;
  spec.expanded = flags.has_expanded_state && flags.is_expanded;
  spec.checked = flags.has_checked_state && flags.is_checked;
  spec.checked_mixed = flags.is_check_state_mixed;
  spec.toggled = flags.has_toggled_state && flags.is_toggled;
  spec.live = flags.is_live_region;
  spec.focusable = flags.is_focusable;
  spec.focused = flags.is_focused;
  spec.required = flags.is_required;
  spec.a11y_focus_blocked = flags.is_accessibility_focus_blocked;

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
  spec.action_scroll_to_offset =
      Has(actions, kFlutterSemanticsActionScrollToOffset);
  spec.action_expand = Has(actions, kFlutterSemanticsActionExpand);
  spec.action_collapse = Has(actions, kFlutterSemanticsActionCollapse);

  return spec;
}

}  // namespace

NodeSpec Translate(int32_t id,
                   const FlutterSemanticsFlags* flags,
                   FlutterSemanticsFlag legacy_flags,
                   FlutterSemanticsAction actions,
                   bool has_label,
                   bool has_children) {
  return TranslateView(
      id, flags != nullptr ? FromStruct(*flags) : FromLegacy(legacy_flags),
      actions, has_label, has_children);
}

NodeSpec Translate(int32_t id,
                   FlutterSemanticsFlag flags,
                   FlutterSemanticsAction actions,
                   bool has_label,
                   bool has_children) {
  return TranslateView(id, FromLegacy(flags), actions, has_label, has_children);
}

}  // namespace accessibility
