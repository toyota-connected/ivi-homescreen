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

#include <cstdint>

#include "gtest/gtest.h"

#include "accessibility/semantics_translator.h"

using accessibility::NodeSpec;
using accessibility::Role;
using accessibility::Translate;

namespace {

FlutterSemanticsFlag Flags(uint32_t bits) {
  return static_cast<FlutterSemanticsFlag>(bits);
}
FlutterSemanticsAction Actions(uint32_t bits) {
  return static_cast<FlutterSemanticsAction>(bits);
}
constexpr FlutterSemanticsAction kNoAction =
    static_cast<FlutterSemanticsAction>(0);

// A non-root (id=1) node with the given flags, no actions, no children/label
// unless overridden — the common shape for the role/state tables below.
NodeSpec NonRoot(uint32_t flags,
                 bool has_label = false,
                 bool has_children = false) {
  return Translate(1, Flags(flags), kNoAction, has_label, has_children);
}

}  // namespace

// ─── Role table (§4.4, first match wins) ───────────────────────────────────

TEST(SemanticsTranslator, RootIsWindowRegardlessOfFlags) {
  // id 0 wins over any flag.
  EXPECT_EQ(
      Translate(0, Flags(kFlutterSemanticsFlagIsButton), kNoAction, false, true)
          .role,
      Role::kWindow);
}

TEST(SemanticsTranslator, TextInputVariants) {
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsTextField).role, Role::kTextInput);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsTextField |
                    kFlutterSemanticsFlagIsMultiline)
                .role,
            Role::kMultilineTextInput);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsTextField |
                    kFlutterSemanticsFlagIsObscured)
                .role,
            Role::kPasswordInput);
  // Obscured takes precedence over multiline.
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsTextField |
                    kFlutterSemanticsFlagIsObscured |
                    kFlutterSemanticsFlagIsMultiline)
                .role,
            Role::kPasswordInput);
}

TEST(SemanticsTranslator, CheckboxVsRadio) {
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagHasCheckedState).role,
            Role::kCheckBox);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagHasCheckedState |
                    kFlutterSemanticsFlagIsInMutuallyExclusiveGroup)
                .role,
            Role::kRadioButton);
}

TEST(SemanticsTranslator, SingleFlagRoles) {
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagHasToggledState).role, Role::kSwitch);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsSlider).role, Role::kSlider);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsButton).role, Role::kButton);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsLink).role, Role::kLink);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsImage).role, Role::kImage);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsHeader).role, Role::kHeading);
  // Keyboard key is a documented approximation to button.
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagIsKeyboardKey).role, Role::kButton);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagHasImplicitScrolling).role,
            Role::kScrollView);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagScopesRoute).role, Role::kPane);
  EXPECT_EQ(NonRoot(kFlutterSemanticsFlagNamesRoute).role, Role::kPane);
}

TEST(SemanticsTranslator, LabelOnlyLeafVsGenericContainer) {
  EXPECT_EQ(Translate(1, Flags(0), kNoAction, /*has_label=*/true,
                      /*has_children=*/false)
                .role,
            Role::kLabel);
  // A labelled node with children is a container, not a label.
  EXPECT_EQ(Translate(1, Flags(0), kNoAction, true, true).role,
            Role::kGenericContainer);
  // No label, no distinguishing flag -> generic container.
  EXPECT_EQ(Translate(1, Flags(0), kNoAction, false, false).role,
            Role::kGenericContainer);
}

TEST(SemanticsTranslator, RolePrecedenceTextFieldBeatsButton) {
  // Both text-field and button set: text field is checked first.
  EXPECT_EQ(
      NonRoot(kFlutterSemanticsFlagIsTextField | kFlutterSemanticsFlagIsButton)
          .role,
      Role::kTextInput);
}

// ─── State mapping ──────────────────────────────────────────────────────────

TEST(SemanticsTranslator, Disabled) {
  EXPECT_FALSE(NonRoot(0).disabled);
  EXPECT_FALSE(NonRoot(kFlutterSemanticsFlagHasEnabledState |
                       kFlutterSemanticsFlagIsEnabled)
                   .disabled);
  // Has an enabled state and is not enabled -> disabled.
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagHasEnabledState).disabled);
  // IsEnabled without HasEnabledState is meaningless -> not disabled.
  EXPECT_FALSE(NonRoot(kFlutterSemanticsFlagIsEnabled).disabled);
}

TEST(SemanticsTranslator, CheckedToggledTruthTable) {
  EXPECT_FALSE(NonRoot(kFlutterSemanticsFlagHasCheckedState).checked);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagHasCheckedState |
                      kFlutterSemanticsFlagIsChecked)
                  .checked);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsCheckStateMixed).checked_mixed);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagHasToggledState |
                      kFlutterSemanticsFlagIsToggled)
                  .toggled);
  EXPECT_FALSE(NonRoot(kFlutterSemanticsFlagHasToggledState).toggled);
}

TEST(SemanticsTranslator, MiscStates) {
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsHidden).hidden);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsReadOnly).read_only);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsSelected).selected);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagHasExpandedState |
                      kFlutterSemanticsFlagIsExpanded)
                  .expanded);
  EXPECT_FALSE(NonRoot(kFlutterSemanticsFlagHasExpandedState).expanded);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsLiveRegion).live);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsFocusable).focusable);
  EXPECT_TRUE(NonRoot(kFlutterSemanticsFlagIsFocused).focused);
}

// ─── Action bitmask mapping ─────────────────────────────────────────────────

TEST(SemanticsTranslator, ActionsPerBit) {
  auto a = [](uint32_t bits) {
    return Translate(1, Flags(0), Actions(bits), false, false);
  };
  EXPECT_TRUE(a(kFlutterSemanticsActionTap).action_tap);
  EXPECT_TRUE(a(kFlutterSemanticsActionLongPress).action_long_press);
  EXPECT_TRUE(a(kFlutterSemanticsActionIncrease).action_increase);
  EXPECT_TRUE(a(kFlutterSemanticsActionDecrease).action_decrease);
  EXPECT_TRUE(a(kFlutterSemanticsActionScrollUp).action_scroll_up);
  EXPECT_TRUE(a(kFlutterSemanticsActionScrollDown).action_scroll_down);
  EXPECT_TRUE(a(kFlutterSemanticsActionScrollLeft).action_scroll_left);
  EXPECT_TRUE(a(kFlutterSemanticsActionScrollRight).action_scroll_right);
  EXPECT_TRUE(a(kFlutterSemanticsActionShowOnScreen).action_show_on_screen);
  EXPECT_TRUE(a(kFlutterSemanticsActionSetText).action_set_text);
  EXPECT_TRUE(a(kFlutterSemanticsActionFocus).action_focus);
}

TEST(SemanticsTranslator, ActionsCombinedAndUnset) {
  const NodeSpec s = Translate(
      1, Flags(0),
      Actions(kFlutterSemanticsActionTap | kFlutterSemanticsActionIncrease),
      false, false);
  EXPECT_TRUE(s.action_tap);
  EXPECT_TRUE(s.action_increase);
  EXPECT_FALSE(s.action_decrease);
  EXPECT_FALSE(s.action_scroll_up);
}

TEST(SemanticsTranslator, EmptyNodeIsInertGenericContainer) {
  const NodeSpec s = Translate(1, Flags(0), kNoAction, false, false);
  EXPECT_EQ(s.role, Role::kGenericContainer);
  EXPECT_FALSE(s.disabled);
  EXPECT_FALSE(s.checked);
  EXPECT_FALSE(s.selected);
  EXPECT_FALSE(s.action_tap);
  EXPECT_FALSE(s.focusable);
}
