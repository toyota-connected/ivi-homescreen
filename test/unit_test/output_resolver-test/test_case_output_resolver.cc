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

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "display/output.h"

using homescreen::BackendFamily;
using homescreen::ClassifyOutputTransition;
using homescreen::OutputEvent;
using homescreen::OutputInfo;
using homescreen::OutputMatch;
using homescreen::OutputResolution;
using homescreen::OutputTransition;
using homescreen::ResolveOutput;
using homescreen::ResolveOutputDetailed;

namespace {

OutputInfo MakeOutput(std::string name,
                      const bool connected = true,
                      std::optional<std::string> serial = std::nullopt) {
  OutputInfo o;
  o.name = std::move(name);
  o.connected = connected;
  o.serial = std::move(serial);
  return o;
}

// A typical two-monitor desktop, both connected.
std::vector<OutputInfo> TwoHeads() {
  return {MakeOutput("DP-1"), MakeOutput("HDMI-A-1")};
}

const std::optional<std::string> kParked = std::nullopt;

// A connector carrying an integrator-assigned role name.
OutputInfo WithRole(std::string name, std::string role, bool connected = true) {
  OutputInfo o = MakeOutput(std::move(name), connected);
  o.output_id = std::move(role);
  return o;
}

// The two verification boards: the panel filling the same role is DSI-1 on a
// Pi 4 and DSI-2 on a Pi 5, and neither publishes an EDID.
std::vector<OutputInfo> Pi4Heads() {
  return {WithRole("DSI-1", "cluster"), WithRole("HDMI-A-1", "center-stack")};
}
std::vector<OutputInfo> Pi5Heads() {
  return {WithRole("DSI-2", "cluster")};
}

}  // namespace

// ---- name matching (the primary path) ----

TEST(OutputResolver, DrmConnectorNameMatches) {
  OutputMatch m;
  m.drm_connector = "HDMI-A-1";
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kDrm),
            std::optional<std::string>("HDMI-A-1"));
}

TEST(OutputResolver, WaylandNameMatches) {
  OutputMatch m;
  m.wl_name = "DP-1";
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kWayland),
            std::optional<std::string>("DP-1"));
}

// The compared field is per-backend: a DRM connector field is ignored on
// Wayland, where wl_name is the discriminator (and vice-versa).
TEST(OutputResolver, NameFieldIsBackendSpecific) {
  OutputMatch m;
  m.drm_connector = "DP-1";  // only the DRM field is set
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kWayland), kParked);
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kDrm),
            std::optional<std::string>("DP-1"));
}

// Identical monitors share make/model — each still binds to its own port.
TEST(OutputResolver, IdenticalMonitorsFollowPort) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1"), MakeOutput("DP-2")};
  outs[0].make = outs[1].make = "GSM";
  outs[0].model = outs[1].model = "LG HDR DQHD";
  OutputMatch a;
  a.drm_connector = "DP-1";
  OutputMatch b;
  b.drm_connector = "DP-2";
  EXPECT_EQ(ResolveOutput(a, outs, BackendFamily::kDrm),
            std::optional<std::string>("DP-1"));
  EXPECT_EQ(ResolveOutput(b, outs, BackendFamily::kDrm),
            std::optional<std::string>("DP-2"));
}

// ---- parked when absent / ineligible ----

TEST(OutputResolver, NamedButAbsentIsParked) {
  OutputMatch m;
  m.drm_connector = "DP-9";  // not present
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kDrm), kParked);
}

TEST(OutputResolver, DisconnectedOutputIsNotEligible) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1", /*connected=*/false)};
  OutputMatch m;
  m.drm_connector = "DP-1";
  EXPECT_EQ(ResolveOutput(m, outs, BackendFamily::kDrm), kParked);
}

TEST(OutputResolver, EmptyMatchIsParked) {
  OutputMatch m;  // nothing set
  EXPECT_TRUE(m.empty());
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kDrm), kParked);
}

// ---- EDID serial (DRM only, must uniquely identify one output) ----

TEST(OutputResolver, UniqueSerialMatches) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1", true, "SN-AAA"),
                                  MakeOutput("DP-2", true, "SN-BBB")};
  OutputMatch m;
  m.edid_serial = "SN-BBB";
  EXPECT_EQ(ResolveOutput(m, outs, BackendFamily::kDrm),
            std::optional<std::string>("DP-2"));
}

// A unique serial takes precedence over a name pointing elsewhere.
TEST(OutputResolver, SerialBeatsName) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1", true, "SN-AAA"),
                                  MakeOutput("DP-2", true, "SN-BBB")};
  OutputMatch m;
  m.edid_serial = "SN-BBB";  // on DP-2
  m.drm_connector = "DP-1";  // points elsewhere
  EXPECT_EQ(ResolveOutput(m, outs, BackendFamily::kDrm),
            std::optional<std::string>("DP-2"));
}

// A duplicate (ambiguous) serial falls back to the connector name.
TEST(OutputResolver, AmbiguousSerialFallsBackToName) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1", true, "DUP"),
                                  MakeOutput("DP-2", true, "DUP")};
  OutputMatch m;
  m.edid_serial = "DUP";
  m.drm_connector = "DP-2";
  EXPECT_EQ(ResolveOutput(m, outs, BackendFamily::kDrm),
            std::optional<std::string>("DP-2"));
}

// Serial is DRM-only: ignored on Wayland (which never exposes serial).
TEST(OutputResolver, SerialIgnoredOnWayland) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1", true, "SN-AAA")};
  OutputMatch m;
  m.edid_serial = "SN-AAA";  // would match on DRM
  EXPECT_EQ(ResolveOutput(m, outs, BackendFamily::kWayland), kParked);
}

// ---- index (deprecated last resort) ----

TEST(OutputResolver, IndexSelectsConnectedInOrder) {
  std::vector<OutputInfo> outs = {MakeOutput("DP-1", /*connected=*/false),
                                  MakeOutput("HDMI-A-1", true),  // index 0
                                  MakeOutput("DP-2", true)};     // index 1
  OutputMatch m;
  m.index = 1;
  EXPECT_EQ(ResolveOutput(m, outs, BackendFamily::kDrm),
            std::optional<std::string>("DP-2"));
}

TEST(OutputResolver, IndexOutOfRangeIsParked) {
  OutputMatch m;
  m.index = 5;
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kDrm), kParked);
}

// The reason output_id exists: one config, two boards, different connector
// names on different cards, and no EDID on either panel.
TEST(OutputResolver, RoleNameBindsAcrossBoards) {
  OutputMatch m;
  m.output_id = "cluster";
  EXPECT_EQ(ResolveOutput(m, Pi4Heads(), BackendFamily::kDrm), "DSI-1");
  EXPECT_EQ(ResolveOutput(m, Pi5Heads(), BackendFamily::kDrm), "DSI-2");
}

// The resolver itself is family-agnostic: unlike edid_serial it does not check
// the backend family, so it will work the day a Wayland provider populates
// output_id. Note that none does today -- only DrmOutputProvider fills the
// field -- so this covers the matching rule, not end-to-end Wayland support.
TEST(OutputResolver, RoleMatchingDoesNotDependOnBackendFamily) {
  OutputMatch m;
  m.output_id = "cluster";
  EXPECT_EQ(ResolveOutput(m, Pi4Heads(), BackendFamily::kWayland), "DSI-1");
}

// Above the serial: assigned deliberately, and it works where EDID does not.
TEST(OutputResolver, RoleNameBeatsSerial) {
  OutputMatch m;
  m.output_id = "cluster";
  m.edid_serial = "SN-HDMI";
  std::vector<OutputInfo> heads = {
      WithRole("DSI-1", "cluster"),
      MakeOutput("HDMI-A-1", true, "SN-HDMI"),
  };
  EXPECT_EQ(ResolveOutput(m, heads, BackendFamily::kDrm), "DSI-1");
}

// A named role that is not present does not fall through to the lower tiers,
// even when one of them would have matched. Binding an instrument cluster by
// port because its role was absent is not a graceful degradation.
//
// Note the scope: this is the resolver refusing to substitute another tier.
// Whether the *view* then goes dark is the caller's decision -- today
// ResolveForView's callers fall back to the backend default, and real parking
// arrives with the hotplug work.
TEST(OutputResolver, MissingRoleDoesNotFallThroughToLowerTiers) {
  OutputMatch m;
  m.output_id = "passenger";
  m.drm_connector = "DSI-1";  // would have matched, and must not rescue it
  EXPECT_EQ(ResolveOutput(m, Pi4Heads(), BackendFamily::kDrm), kParked);
}

// A disconnected port carrying the role is not a place to put a view.
TEST(OutputResolver, DisconnectedRoleIsNotEligible) {
  OutputMatch m;
  m.output_id = "passenger";
  std::vector<OutputInfo> heads = {WithRole("HDMI-A-2", "passenger", false)};
  EXPECT_EQ(ResolveOutput(m, heads, BackendFamily::kDrm), kParked);
}

// Without a udev rule nothing carries a role, and every existing config must
// behave exactly as it did before this tier existed.
TEST(OutputResolver, NoRuleInstalledLeavesOtherTiersUntouched) {
  OutputMatch m;
  m.drm_connector = "HDMI-A-1";
  EXPECT_EQ(ResolveOutput(m, TwoHeads(), BackendFamily::kDrm), "HDMI-A-1");

  OutputMatch by_serial;
  by_serial.edid_serial = "SN-1";
  std::vector<OutputInfo> heads = {MakeOutput("DP-1", true, "SN-1"),
                                   MakeOutput("HDMI-A-1")};
  EXPECT_EQ(ResolveOutput(by_serial, heads, BackendFamily::kDrm), "DP-1");
}

// An output_id constraint is a constraint: empty() must not report otherwise,
// or a view naming only a role would be treated as unconstrained.
TEST(OutputResolver, RoleNameCountsAsAConstraint) {
  OutputMatch m;
  m.output_id = "cluster";
  EXPECT_FALSE(m.empty());
}

// A duplicated role is a mistake in the rule, and the enumeration order that
// would otherwise decide it is not stable. Park rather than guess.
TEST(OutputResolver, DuplicateRoleIsAmbiguousAndParks) {
  OutputMatch m;
  m.output_id = "cluster";
  std::vector<OutputInfo> heads = {WithRole("DSI-1", "cluster"),
                                   WithRole("HDMI-A-1", "cluster")};
  EXPECT_EQ(ResolveOutput(m, heads, BackendFamily::kDrm), kParked);
}

// An empty output_id is not a constraint that matches nothing; the parser
// leaves it unset, so a view carrying one is unconstrained rather than parked.
TEST(OutputResolver, EmptyRoleIsNotAConstraint) {
  OutputMatch m;
  m.output_id = std::nullopt;
  EXPECT_TRUE(m.empty());
}

// The distinction a bare optional cannot carry: asking for nothing and asking
// for something absent both yield no name, but call for opposite handling.
TEST(OutputResolver, DetailedResultSeparatesUnconstrainedFromUnresolved) {
  const OutputMatch none;
  EXPECT_EQ(ResolveOutputDetailed(none, TwoHeads(), BackendFamily::kDrm).status,
            OutputResolution::Status::kUnconstrained);

  OutputMatch missing_role;
  missing_role.output_id = "passenger";
  const auto r =
      ResolveOutputDetailed(missing_role, Pi4Heads(), BackendFamily::kDrm);
  EXPECT_EQ(r.status, OutputResolution::Status::kUnresolved);
  EXPECT_FALSE(r.bound());

  // The same distinction for the older tiers, not just for roles.
  OutputMatch absent_name;
  absent_name.drm_connector = "HDMI-A-9";
  EXPECT_EQ(ResolveOutputDetailed(absent_name, TwoHeads(), BackendFamily::kDrm)
                .status,
            OutputResolution::Status::kUnresolved);

  OutputMatch present;
  present.drm_connector = "HDMI-A-1";
  const auto ok =
      ResolveOutputDetailed(present, TwoHeads(), BackendFamily::kDrm);
  EXPECT_TRUE(ok.bound());
  EXPECT_EQ(ok.name, "HDMI-A-1");
}

// ---------------------------------------------------------------------------
// ClassifyOutputTransition: what an output event means for one view. Split out
// of App's listener so the decision can be exercised without a display, an
// engine or a compositor.
// ---------------------------------------------------------------------------

namespace {

OutputResolution Bound(std::string name) {
  return {OutputResolution::Status::kBound, std::move(name)};
}
OutputResolution Unconstrained() {
  return {OutputResolution::Status::kUnconstrained, {}};
}
OutputResolution Unresolved() {
  return {OutputResolution::Status::kUnresolved, {}};
}

}  // namespace

TEST(OutputTransitions, StayingPutIsNotAnEvent) {
  // The common case by far: something changed elsewhere on the display.
  EXPECT_EQ(
      ClassifyOutputTransition(std::optional<std::string>{"DSI-1"},
                               Bound("DSI-1"), OutputEvent::kAdded, "HDMI-A-1"),
      OutputTransition::kNone);
}

TEST(OutputTransitions, ConstraintNowResolvesElsewhere) {
  EXPECT_EQ(
      ClassifyOutputTransition(std::optional<std::string>{"HDMI-A-1"},
                               Bound("DSI-1"), OutputEvent::kAdded, "DSI-1"),
      OutputTransition::kMoved);
}

TEST(OutputTransitions, AnUnboundViewGainsAnOutput) {
  EXPECT_EQ(ClassifyOutputTransition(std::nullopt, Bound("DSI-1"),
                                     OutputEvent::kAdded, "DSI-1"),
            OutputTransition::kAppeared);
}

TEST(OutputTransitions, AConstraintThatStopsResolvingLosesTheOutput) {
  EXPECT_EQ(
      ClassifyOutputTransition(std::optional<std::string>{"HDMI-A-1"},
                               Unresolved(), OutputEvent::kRemoved, "HDMI-A-1"),
      OutputTransition::kLost);
}

TEST(OutputTransitions, AnUnconstrainedViewIsNeverLost) {
  // No constraint means the backend's own pick stands, and re-resolving says
  // nothing about it -- reporting "lost" here would park a view that is still
  // perfectly placed.
  EXPECT_EQ(ClassifyOutputTransition(std::optional<std::string>{"DSI-1"},
                                     Unconstrained(), OutputEvent::kRemoved,
                                     "HDMI-A-1"),
            OutputTransition::kNone);
}

TEST(OutputTransitions, ReconfiguringTheViewsOwnOutputCounts) {
  EXPECT_EQ(
      ClassifyOutputTransition(std::optional<std::string>{"DSI-1"},
                               Bound("DSI-1"), OutputEvent::kChanged, "DSI-1"),
      OutputTransition::kReconfigured);
  // ... and an unconstrained view on that output is equally affected: its mode
  // may have changed underneath it.
  EXPECT_EQ(
      ClassifyOutputTransition(std::optional<std::string>{"DSI-1"},
                               Unconstrained(), OutputEvent::kChanged, "DSI-1"),
      OutputTransition::kReconfigured);
}

TEST(OutputTransitions, ReconfiguringSomeOtherOutputDoesNot) {
  EXPECT_EQ(ClassifyOutputTransition(std::optional<std::string>{"DSI-1"},
                                     Bound("DSI-1"), OutputEvent::kChanged,
                                     "HDMI-A-1"),
            OutputTransition::kNone);
}

TEST(OutputTransitions, AViewWithNoBindingAndNoConstraintHasNothingToDo) {
  EXPECT_EQ(ClassifyOutputTransition(std::nullopt, Unconstrained(),
                                     OutputEvent::kChanged, "DSI-1"),
            OutputTransition::kNone);
}
