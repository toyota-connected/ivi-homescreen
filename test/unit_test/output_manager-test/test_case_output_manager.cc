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

// Unit tests for homescreen::OutputManager::ResolveForView.
//
// The function reads [view.output] from a Configuration::Config, queries a
// display's IOutputProvider for live outputs, and returns the matched output
// name (or nullopt).  All dependencies are exercised through GMock fakes; no
// Wayland / DRM connection is opened.

#include <optional>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "configuration/configuration.h"
#include "display/output.h"
#include "display/output_manager.h"
#include "mock_display.h"

using homescreen::BackendFamily;
using homescreen::OutputInfo;
using homescreen::OutputManager;
using homescreen::OutputMatch;
using testing::NiceMock;
using testing::Return;

namespace {

// Build a minimal connected OutputInfo with the given name.
OutputInfo MakeOutput(const std::string& name, bool connected = true) {
  OutputInfo o;
  o.name = name;
  o.connected = connected;
  return o;
}

// A Configuration::Config with no [view.output] constraint.
Configuration::Config EmptyMatchConfig() {
  Configuration::Config cfg;
  // view.output is default-constructed; OutputMatch::empty() == true.
  return cfg;
}

// A Configuration::Config whose [view.output] specifies a DRM connector.
Configuration::Config DrmConfig(const std::string& connector) {
  Configuration::Config cfg;
  cfg.view.output.drm_connector = connector;
  return cfg;
}

// A Configuration::Config whose [view.output] specifies a Wayland wl_name.
Configuration::Config WlConfig(const std::string& wl_name) {
  Configuration::Config cfg;
  cfg.view.output.wl_name = wl_name;
  return cfg;
}

}  // namespace

// ---- No constraint --------------------------------------------------------

TEST(OutputManager, NoOutputConstraint_ReturnsNullopt) {
  // An empty OutputMatch means "use the backend default" — ResolveForView
  // must return nullopt without touching the display.
  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).Times(0);

  const auto result = OutputManager::ResolveForView(
      EmptyMatchConfig(), &display, BackendFamily::kDrm);
  EXPECT_EQ(result, std::nullopt);
}

// ---- Null display ---------------------------------------------------------

TEST(OutputManager, NullDisplay_ReturnsNullopt) {
  // A null display pointer → no provider → nullopt (with a warning logged).
  const Configuration::Config cfg = DrmConfig("DP-1");
  const auto result =
      OutputManager::ResolveForView(cfg, nullptr, BackendFamily::kDrm);
  EXPECT_EQ(result, std::nullopt);
}

// ---- Null output provider -------------------------------------------------

TEST(OutputManager, NullOutputProvider_ReturnsNullopt) {
  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).WillOnce(Return(nullptr));

  const Configuration::Config cfg = DrmConfig("DP-1");
  const auto result =
      OutputManager::ResolveForView(cfg, &display, BackendFamily::kDrm);
  EXPECT_EQ(result, std::nullopt);
}

// ---- DRM connector match --------------------------------------------------

TEST(OutputManager, DrmConnectorMatch_ReturnsName) {
  MockIOutputProvider provider;
  EXPECT_CALL(provider, EnumerateOutputs())
      .WillOnce(Return(
          std::vector<OutputInfo>{MakeOutput("DP-1"), MakeOutput("HDMI-A-1")}));

  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).WillOnce(Return(&provider));

  const Configuration::Config cfg = DrmConfig("HDMI-A-1");
  const auto result =
      OutputManager::ResolveForView(cfg, &display, BackendFamily::kDrm);
  EXPECT_EQ(result, std::optional<std::string>("HDMI-A-1"));
}

// ---- Wayland name match ---------------------------------------------------

TEST(OutputManager, WaylandNameMatch_ReturnsName) {
  MockIOutputProvider provider;
  EXPECT_CALL(provider, EnumerateOutputs())
      .WillOnce(Return(
          std::vector<OutputInfo>{MakeOutput("DP-1"), MakeOutput("HDMI-A-1")}));

  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).WillOnce(Return(&provider));

  const Configuration::Config cfg = WlConfig("DP-1");
  const auto result =
      OutputManager::ResolveForView(cfg, &display, BackendFamily::kWayland);
  EXPECT_EQ(result, std::optional<std::string>("DP-1"));
}

// ---- Missing output -------------------------------------------------------

TEST(OutputManager, MissingOutput_ReturnsNullopt) {
  MockIOutputProvider provider;
  EXPECT_CALL(provider, EnumerateOutputs())
      .WillOnce(Return(std::vector<OutputInfo>{MakeOutput("DP-1")}));

  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).WillOnce(Return(&provider));

  const Configuration::Config cfg = DrmConfig("HDMI-A-1");
  const auto result =
      OutputManager::ResolveForView(cfg, &display, BackendFamily::kDrm);
  EXPECT_EQ(result, std::nullopt);
}

// ---- Wrong backend family -------------------------------------------------

TEST(OutputManager, WrongFamilyField_ReturnsNullopt) {
  // drm_connector is set but family is kWayland — the DRM field is ignored on
  // the Wayland path, so nothing matches.
  MockIOutputProvider provider;
  EXPECT_CALL(provider, EnumerateOutputs())
      .WillOnce(Return(std::vector<OutputInfo>{MakeOutput("DP-1")}));

  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).WillOnce(Return(&provider));

  const Configuration::Config cfg = DrmConfig("DP-1");
  const auto result =
      OutputManager::ResolveForView(cfg, &display, BackendFamily::kWayland);
  EXPECT_EQ(result, std::nullopt);
}

// ---- Empty output set -----------------------------------------------------

TEST(OutputManager, EmptyOutputSet_ReturnsNullopt) {
  MockIOutputProvider provider;
  EXPECT_CALL(provider, EnumerateOutputs())
      .WillOnce(Return(std::vector<OutputInfo>{}));

  NiceMock<MockIDisplay> display;
  EXPECT_CALL(display, GetOutputProvider()).WillOnce(Return(&provider));

  const Configuration::Config cfg = DrmConfig("DP-1");
  const auto result =
      OutputManager::ResolveForView(cfg, &display, BackendFamily::kDrm);
  EXPECT_EQ(result, std::nullopt);
}
