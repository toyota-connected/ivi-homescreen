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
using homescreen::OutputInfo;
using homescreen::OutputMatch;
using homescreen::ResolveOutput;

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
