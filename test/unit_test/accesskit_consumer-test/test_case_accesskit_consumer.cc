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
#include <set>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs_semantics.h"

#include "accessibility/accesskit_consumer.h"

using accessibility::ToAccessKitRole;
using accessibility::ToIhsAction;

namespace {

// Every role the hub can publish. Kept as an explicit list rather than a range
// so that adding a role to the ABI without deciding what a screen reader
// should call it fails here.
const std::vector<IhsSemanticsRole>& AllRoles() {
  static const std::vector<IhsSemanticsRole> kRoles = {
      IHS_SEMANTICS_ROLE_UNKNOWN,
      IHS_SEMANTICS_ROLE_WINDOW,
      IHS_SEMANTICS_ROLE_BUTTON,
      IHS_SEMANTICS_ROLE_TEXT_INPUT,
      IHS_SEMANTICS_ROLE_MULTILINE_TEXT_INPUT,
      IHS_SEMANTICS_ROLE_PASSWORD_INPUT,
      IHS_SEMANTICS_ROLE_SLIDER,
      IHS_SEMANTICS_ROLE_SWITCH,
      IHS_SEMANTICS_ROLE_CHECK_BOX,
      IHS_SEMANTICS_ROLE_RADIO_BUTTON,
      IHS_SEMANTICS_ROLE_LINK,
      IHS_SEMANTICS_ROLE_IMAGE,
      IHS_SEMANTICS_ROLE_HEADING,
      IHS_SEMANTICS_ROLE_SCROLL_VIEW,
      IHS_SEMANTICS_ROLE_PANE,
      IHS_SEMANTICS_ROLE_LABEL,
      IHS_SEMANTICS_ROLE_GENERIC_CONTAINER,
  };
  return kRoles;
}

}  // namespace

// Only the unknown role may land on ACCESSKIT_ROLE_UNKNOWN. A real role
// collapsing to unknown is how a control ends up announced as nothing in
// particular, which is invisible without an assistive technology attached.
TEST(AccessKitConsumer, EveryKnownRoleMapsToSomethingSpecific) {
  for (const IhsSemanticsRole role : AllRoles()) {
    const accesskit_role mapped = ToAccessKitRole(role);
    if (role == IHS_SEMANTICS_ROLE_UNKNOWN) {
      EXPECT_EQ(mapped, ACCESSKIT_ROLE_UNKNOWN);
    } else {
      EXPECT_NE(mapped, ACCESSKIT_ROLE_UNKNOWN)
          << "hub role " << static_cast<int>(role)
          << " has no AccessKit equivalent";
    }
  }
}

// Two distinct roles mapping to the same AccessKit role would make a screen
// reader unable to tell the controls apart.
TEST(AccessKitConsumer, DistinctRolesMapDistinctly) {
  std::set<int> seen;
  for (const IhsSemanticsRole role : AllRoles()) {
    if (role == IHS_SEMANTICS_ROLE_UNKNOWN) {
      continue;
    }
    const int mapped = static_cast<int>(ToAccessKitRole(role));
    EXPECT_TRUE(seen.insert(mapped).second)
        << "AccessKit role " << mapped << " is claimed by two hub roles";
  }
}

TEST(AccessKitConsumer, RolesMapToTheExpectedControls) {
  EXPECT_EQ(ToAccessKitRole(IHS_SEMANTICS_ROLE_BUTTON), ACCESSKIT_ROLE_BUTTON);
  EXPECT_EQ(ToAccessKitRole(IHS_SEMANTICS_ROLE_SLIDER), ACCESSKIT_ROLE_SLIDER);
  EXPECT_EQ(ToAccessKitRole(IHS_SEMANTICS_ROLE_CHECK_BOX),
            ACCESSKIT_ROLE_CHECK_BOX);
  // A password field must not be announced as an ordinary text input: the
  // distinction is what makes a screen reader suppress the characters.
  EXPECT_EQ(ToAccessKitRole(IHS_SEMANTICS_ROLE_PASSWORD_INPUT),
            ACCESSKIT_ROLE_PASSWORD_INPUT);
  EXPECT_NE(ToAccessKitRole(IHS_SEMANTICS_ROLE_PASSWORD_INPUT),
            ToAccessKitRole(IHS_SEMANTICS_ROLE_TEXT_INPUT));
}

// The actions a screen reader actually issues have to reach the framework.
TEST(AccessKitConsumer, ScreenReaderActionsMapOntoHubActions) {
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_CLICK), IHS_SEMANTICS_ACTION_TAP);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_INCREMENT),
            IHS_SEMANTICS_ACTION_INCREASE);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_DECREMENT),
            IHS_SEMANTICS_ACTION_DECREASE);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_SCROLL_INTO_VIEW),
            IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_EXPAND), IHS_SEMANTICS_ACTION_EXPAND);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_COLLAPSE),
            IHS_SEMANTICS_ACTION_COLLAPSE);
}

// Focus and blur are the accessibility cursor moving. They are the reason this
// consumer registers with the full allow mask, so they must map -- if they
// came back as 0 the cursor would never move and the tree would be unwalkable.
TEST(AccessKitConsumer, AccessibilityFocusActionsMap) {
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_FOCUS),
            IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_BLUR),
            IHS_SEMANTICS_ACTION_DID_LOSE_A11Y_FOCUS);
  // And they are inside the mask this consumer asks for.
  EXPECT_NE(IHS_SEMANTICS_ACTION_ALL & IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS,
            0u);
}

// An action AccessKit offers that the framework has no equivalent for reports
// 0, so the caller can say so rather than dispatching an arbitrary bit.
TEST(AccessKitConsumer, UnmappedActionsReportZero) {
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_HIDE_TOOLTIP), 0u);
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_REPLACE_SELECTED_TEXT), 0u);
}

// Every action this consumer maps must be one a hub consumer is allowed to
// hold, or the dispatch would be refused at the funnel every time.
TEST(AccessKitConsumer, EveryMappedActionIsWithinTheAllowMask) {
  const std::vector<accesskit_action> kActions = {
      ACCESSKIT_ACTION_CLICK,       ACCESSKIT_ACTION_FOCUS,
      ACCESSKIT_ACTION_BLUR,        ACCESSKIT_ACTION_INCREMENT,
      ACCESSKIT_ACTION_DECREMENT,   ACCESSKIT_ACTION_SCROLL_INTO_VIEW,
      ACCESSKIT_ACTION_SCROLL_UP,   ACCESSKIT_ACTION_SCROLL_DOWN,
      ACCESSKIT_ACTION_SCROLL_LEFT, ACCESSKIT_ACTION_SCROLL_RIGHT,
      ACCESSKIT_ACTION_EXPAND,      ACCESSKIT_ACTION_COLLAPSE,
  };
  for (const accesskit_action action : kActions) {
    const uint64_t mapped = ToIhsAction(action);
    ASSERT_NE(mapped, 0u) << "action " << static_cast<int>(action)
                          << " lost its mapping";
    EXPECT_EQ(mapped & ~IHS_SEMANTICS_ACTION_ALL, 0u)
        << "mapped action 0x" << std::hex << mapped
        << " is not a bit this ABI defines";
  }
}
