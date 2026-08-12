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
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs_semantics.h"
#include "ihs/ihs_semantics_host.h"

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

namespace {

// Publishes a one-node tree and hands back the built AccessKit node. The
// snapshot is real rather than a stub, since BuildNode resolves children and
// custom-action declarations through it.
class BuiltNode {
 public:
  BuiltNode(const IhsSemanticsPublishNode& node,
            std::vector<IhsSemanticsPublishCustomAction> customs = {}) {
    IhsSemanticsPublishInfo info{};
    info.struct_size = sizeof(info);
    info.nodes = &node;
    info.node_count = 1;
    info.custom_actions = customs.empty() ? nullptr : customs.data();
    info.custom_action_count = customs.size();
    ihs_semantics_publish(&info);
    snapshot_ = ihs_semantics_acquire_snapshot();
    built_ = accessibility::BuildNode(
        ihs_semantics_snapshot_node_at(snapshot_, 0), snapshot_);
  }
  ~BuiltNode() {
    accesskit_node_free(built_);
    ihs_semantics_release_snapshot(snapshot_);
    ihs_semantics_clear();
  }
  BuiltNode(const BuiltNode&) = delete;
  BuiltNode& operator=(const BuiltNode&) = delete;

  accesskit_node* get() const { return built_; }

 private:
  const IhsSemanticsSnapshot* snapshot_ = nullptr;
  accesskit_node* built_ = nullptr;
};

// Reads a string getter that hands back an owned copy.
std::string Owned(char* value) {
  if (value == nullptr) {
    return {};
  }
  std::string out(value);
  accesskit_string_free(value);
  return out;
}

IhsSemanticsPublishNode PlainNode(const char* label, IhsSemanticsRole role) {
  IhsSemanticsPublishNode node{};
  node.id = 0;
  node.label = label;
  node.hint = "";
  node.value = "";
  node.tooltip = "";
  node.identifier = "";
  node.role = role;
  return node;
}

}  // namespace

// AccessKit derives a Label node's accessible name from its value, not its
// label. Flutter puts the text in the label, so without the fixup every piece
// of static text in the UI reaches a screen reader nameless -- and since a
// live region's announcement is built from that same name, an unnamed toast is
// never announced at all.
TEST(AccessKitConsumer, LabelNodeCarriesItsTextInTheValue) {
  const IhsSemanticsPublishNode node =
      PlainNode("Now playing", IHS_SEMANTICS_ROLE_LABEL);
  const BuiltNode built(node);
  EXPECT_EQ(Owned(accesskit_node_value(built.get())), "Now playing");
}

// Only a Label takes its name from the value; anything else keeps whatever
// value it was given, so a slider reading "21.5" is not overwritten.
TEST(AccessKitConsumer, NonLabelRolesKeepTheirOwnValue) {
  IhsSemanticsPublishNode node =
      PlainNode("Driver temperature", IHS_SEMANTICS_ROLE_SLIDER);
  node.value = "21.5";
  const BuiltNode built(node);
  EXPECT_EQ(Owned(accesskit_node_value(built.get())), "21.5");

  // And a Label that already has a value keeps it rather than being clobbered.
  IhsSemanticsPublishNode labelled =
      PlainNode("ignored", IHS_SEMANTICS_ROLE_LABEL);
  labelled.value = "explicit";
  const BuiltNode built_label(labelled);
  EXPECT_EQ(Owned(accesskit_node_value(built_label.get())), "explicit");
}

// Focus is what a screen reader uses to place its cursor. Offering it on every
// node -- as this did before -- makes static text and layout containers look
// like focus targets, which reads as a tree full of nothing.
TEST(AccessKitConsumer, FocusIsOfferedOnlyWhereTheNodeCanHoldTheCursor) {
  IhsSemanticsPublishNode focusable =
      PlainNode("Play", IHS_SEMANTICS_ROLE_BUTTON);
  focusable.focusable = true;
  const BuiltNode built_focusable(focusable);
  EXPECT_TRUE(accesskit_node_supports_action(built_focusable.get(),
                                             ACCESSKIT_ACTION_FOCUS));

  const IhsSemanticsPublishNode plain =
      PlainNode("Now playing", IHS_SEMANTICS_ROLE_LABEL);
  const BuiltNode built_plain(plain);
  EXPECT_FALSE(accesskit_node_supports_action(built_plain.get(),
                                              ACCESSKIT_ACTION_FOCUS));
}

// The framework can mark a node as unable to take the accessibility cursor
// even though it is otherwise focusable -- content behind a modal barrier.
// Offering focus anyway would walk a screen reader into sealed-off content.
TEST(AccessKitConsumer, BlockedNodesDoNotOfferFocus) {
  IhsSemanticsPublishNode blocked =
      PlainNode("Behind a dialog", IHS_SEMANTICS_ROLE_BUTTON);
  blocked.focusable = true;
  blocked.a11y_focus_blocked = true;
  const BuiltNode built(blocked);
  EXPECT_FALSE(
      accesskit_node_supports_action(built.get(), ACCESSKIT_ACTION_FOCUS));
}

// A live region is announced without the cursor moving to it, which is the
// only way a toast reaches a screen reader before it disappears.
TEST(AccessKitConsumer, LiveRegionsArePolite) {
  IhsSemanticsPublishNode toast =
      PlainNode("Bluetooth connected", IHS_SEMANTICS_ROLE_LABEL);
  toast.live_region = true;
  const BuiltNode built(toast);
  const accesskit_opt_live live = accesskit_node_live(built.get());
  ASSERT_TRUE(live.has_value);
  EXPECT_EQ(live.value, ACCESSKIT_LIVE_POLITE);

  // An ordinary node says nothing about liveness at all, which is distinct
  // from saying "off": AccessKit inherits liveness from an ancestor when the
  // node itself is silent, and asserting off would suppress that.
  const IhsSemanticsPublishNode quiet =
      PlainNode("Now playing", IHS_SEMANTICS_ROLE_LABEL);
  const BuiltNode built_quiet(quiet);
  EXPECT_FALSE(accesskit_node_live(built_quiet.get()).has_value);
}

// The app's stable id is what an automation client keys on when a label is
// localized or ambiguous. Empty at the 3.38.3 deployment floor, so this pins
// the wiring rather than anything observable on a shipping target today.
TEST(AccessKitConsumer, IdentifierAndTooltipAreForwarded) {
  IhsSemanticsPublishNode node = PlainNode("Play", IHS_SEMANTICS_ROLE_BUTTON);
  node.identifier = "media.play";
  node.tooltip = "Start playback";
  const BuiltNode built(node);
  EXPECT_EQ(Owned(accesskit_node_author_id(built.get())), "media.play");
  EXPECT_EQ(Owned(accesskit_node_tooltip(built.get())), "Start playback");
}

// An unannotated node must not claim an empty identifier: an author id of ""
// is a different statement from having none, and automation clients key on it.
TEST(AccessKitConsumer, UnannotatedNodesCarryNoIdentifier) {
  const IhsSemanticsPublishNode node =
      PlainNode("Play", IHS_SEMANTICS_ROLE_BUTTON);
  const BuiltNode built(node);
  EXPECT_EQ(accesskit_node_author_id(built.get()), nullptr);
  EXPECT_EQ(accesskit_node_tooltip(built.get()), nullptr);
}

// Custom actions are the application's own verbs. A node referencing an
// undeclared id is skipped rather than announced with no name.
TEST(AccessKitConsumer, CustomActionsResolveTheirDeclaredLabels) {
  std::vector<int32_t> ids = {100, 999};  // 999 is never declared
  IhsSemanticsPublishNode node = PlainNode("Play", IHS_SEMANTICS_ROLE_BUTTON);
  node.custom_action_ids = ids.data();
  node.custom_action_count = ids.size();

  std::vector<IhsSemanticsPublishCustomAction> customs(1);
  customs[0].id = 100;
  customs[0].label = "Add to favorites";
  customs[0].hint = "";

  const BuiltNode built(node, customs);
  accesskit_custom_actions* actions =
      accesskit_node_custom_actions(built.get());
  ASSERT_NE(actions, nullptr);
  EXPECT_EQ(actions->length, 1u);
  EXPECT_EQ(accesskit_custom_action_id(actions->values[0]), 100);
  EXPECT_EQ(Owned(accesskit_custom_action_description(actions->values[0])),
            "Add to favorites");
  accesskit_custom_actions_free(actions);
}

// The hub normalizes an absent string to "" so its consumers need no null
// checks. AccessKit's model is optional, where Some("") is not None, so that
// normalization has to be undone at this boundary: an empty label would be
// counted as a label the node does not have, and a live region whose name is
// empty emits an announcement of nothing.
TEST(AccessKitConsumer, EmptyStringsAreNotForwardedAsContent) {
  const IhsSemanticsPublishNode node =
      PlainNode("", IHS_SEMANTICS_ROLE_BUTTON);  // no label, hint, or value
  const BuiltNode built(node);
  EXPECT_EQ(accesskit_node_label(built.get()), nullptr);
  EXPECT_EQ(accesskit_node_description(built.get()), nullptr);
  EXPECT_EQ(accesskit_node_value(built.get()), nullptr);
}

// The Label fixup must not manufacture a value out of an absent label either.
TEST(AccessKitConsumer, LabelNodeWithNoTextCarriesNoValue) {
  const IhsSemanticsPublishNode node = PlainNode("", IHS_SEMANTICS_ROLE_LABEL);
  const BuiltNode built(node);
  EXPECT_EQ(accesskit_node_value(built.get()), nullptr);
}

// The two parameterized actions must be advertised, or an assistive
// technology has no way to ask for them in the first place.
TEST(AccessKitConsumer, ParameterizedActionsAreAdvertised) {
  IhsSemanticsPublishNode field =
      PlainNode("Destination", IHS_SEMANTICS_ROLE_TEXT_INPUT);
  field.actions = IHS_SEMANTICS_ACTION_SET_TEXT;
  const BuiltNode built_field(field);
  // AccessKit spells "replace the text" SetValue.
  EXPECT_TRUE(accesskit_node_supports_action(built_field.get(),
                                             ACCESSKIT_ACTION_SET_VALUE));

  IhsSemanticsPublishNode list =
      PlainNode("Media list", IHS_SEMANTICS_ROLE_SCROLL_VIEW);
  list.actions = IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET;
  const BuiltNode built_list(list);
  EXPECT_TRUE(accesskit_node_supports_action(
      built_list.get(), ACCESSKIT_ACTION_SET_SCROLL_OFFSET));
}

// A node that cannot take them must not claim them: an action offered and
// then refused at the funnel is a worse answer than one never advertised.
TEST(AccessKitConsumer, ParameterizedActionsAreNotClaimedWithoutSupport) {
  const IhsSemanticsPublishNode plain =
      PlainNode("Now playing", IHS_SEMANTICS_ROLE_LABEL);
  const BuiltNode built(plain);
  EXPECT_FALSE(
      accesskit_node_supports_action(built.get(), ACCESSKIT_ACTION_SET_VALUE));
  EXPECT_FALSE(accesskit_node_supports_action(
      built.get(), ACCESSKIT_ACTION_SET_SCROLL_OFFSET));
}

// SetScrollOffset carries a point, so it needs no inference and maps straight
// through. AccessKit's AT-SPI backend does not emit it today -- the numeric
// SetValue path does the work on this platform -- but the mapping is exact
// where it is produced.
TEST(AccessKitConsumer, ScrollOffsetActionMapsToScrollToOffset) {
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_SET_SCROLL_OFFSET),
            IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET);
}

// SetValue is not mapped by tag: its meaning depends on whether a string or a
// number arrived, so the handler decides and this table must not guess.
TEST(AccessKitConsumer, SetValueIsNotMappedByTagAlone) {
  EXPECT_EQ(ToIhsAction(ACCESSKIT_ACTION_SET_VALUE), 0u);
}
