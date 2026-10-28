/*
 * Copyright 2026 Sky
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

// Tests for the runtime MSAA sample count: [global].msaa_samples and
// --msaa-samples.
//
// Ported from flutter-embedder-entos, where the sample count is runtime
// configurable. This embedder previously baked it in at compile time
// (BUILD_EGL_ENABLE_MULTISAMPLE / kMsaaSamples), which meant MSAA -- typically
// the largest single GPU allocation -- could not be tuned or switched off per
// device without a rebuild.
//
// The two entry points differ deliberately: a bad value in TOML is ignored with
// an error (a bad key in an on-device config file must not stop the embedder
// starting), whereas a bad value on the command line is fatal (an operator typo
// is worth surfacing rather than silently rendering at a different cost).

#include "gtest/gtest.h"

#include <string>
#include <vector>

#include <configuration/configuration.h>

namespace {

// ParseArgcArgv resolves a bundle, so every CLI case needs a valid -b.
constexpr char kSourceRoot[] = SOURCE_ROOT_DIR;

// Parses @p doc as the root TOML document and runs the [global] extraction.
Configuration::Config ParseGlobal(const std::string& doc) {
  toml::parse_result parsed = toml::parse(doc);
  EXPECT_TRUE(parsed) << "test TOML failed to parse";
  Configuration::Config config{};
  toml::table tbl = std::move(parsed).table();
  Configuration::get_global_parameters(&tbl, config);
  return config;
}

// Runs the real CLI parser over @p args (argv[0] and -b are added here).
std::vector<Configuration::Config> ParseCli(std::vector<const char*> args) {
  std::vector<const char*> argv;
  argv.push_back("homescreen");
  argv.push_back("-b");
  argv.push_back(kSourceRoot);
  for (auto* a : args) {
    argv.push_back(a);
  }
  return Configuration::ParseArgcArgv(static_cast<int>(argv.size()),
                                      argv.data());
}

}  // namespace

// ---------------------------------------------------------------------------
// [global].msaa_samples
// ---------------------------------------------------------------------------

TEST(HomescreenConfigurationMsaa, TomlAbsentLeavesUnset) {
  const auto config = ParseGlobal("[global]\napp_id = \"test\"\n");
  EXPECT_FALSE(config.msaa_samples.has_value());
}

TEST(HomescreenConfigurationMsaa, TomlAcceptsEveryValidCount) {
  for (const auto samples : {0, 2, 4, 8}) {
    const auto config = ParseGlobal(
        "[global]\nmsaa_samples = " + std::to_string(samples) + "\n");
    ASSERT_TRUE(config.msaa_samples.has_value())
        << "rejected valid count " << samples;
    EXPECT_EQ(*config.msaa_samples, static_cast<uint32_t>(samples));
  }
}

// 0 is meaningful: it is how a device turns MSAA off, and must be
// distinguishable from "not configured".
TEST(HomescreenConfigurationMsaa, TomlZeroIsSetNotAbsent) {
  const auto config = ParseGlobal("[global]\nmsaa_samples = 0\n");
  ASSERT_TRUE(config.msaa_samples.has_value());
  EXPECT_EQ(*config.msaa_samples, 0u);
}

TEST(HomescreenConfigurationMsaa, TomlRejectsOutOfRangeCounts) {
  for (const auto samples : {1, 3, 5, 6, 7, 16, -1, 1000}) {
    const auto config = ParseGlobal(
        "[global]\nmsaa_samples = " + std::to_string(samples) + "\n");
    EXPECT_FALSE(config.msaa_samples.has_value())
        << "accepted invalid count " << samples;
  }
}

TEST(HomescreenConfigurationMsaa, TomlIgnoresWrongType) {
  EXPECT_FALSE(
      ParseGlobal("[global]\nmsaa_samples = \"4\"\n").msaa_samples.has_value());
  EXPECT_FALSE(
      ParseGlobal("[global]\nmsaa_samples = true\n").msaa_samples.has_value());
  EXPECT_FALSE(
      ParseGlobal("[global]\nmsaa_samples = 4.0\n").msaa_samples.has_value());
}

// ---------------------------------------------------------------------------
// --msaa-samples
// ---------------------------------------------------------------------------

TEST(HomescreenConfigurationMsaa, CliAbsentLeavesUnset) {
  const auto configs = ParseCli({});
  ASSERT_FALSE(configs.empty());
  EXPECT_FALSE(configs.back().msaa_samples.has_value());
}

TEST(HomescreenConfigurationMsaa, CliAcceptsEveryValidCount) {
  for (const auto* samples : {"0", "2", "4", "8"}) {
    const auto configs = ParseCli({"--msaa-samples", samples});
    ASSERT_FALSE(configs.empty());
    ASSERT_TRUE(configs.back().msaa_samples.has_value())
        << "rejected valid count " << samples;
    EXPECT_EQ(*configs.back().msaa_samples,
              static_cast<uint32_t>(std::stoi(samples)));
  }
}

TEST(HomescreenConfigurationMsaa, CliZeroDisables) {
  const auto configs = ParseCli({"--msaa-samples", "0"});
  ASSERT_FALSE(configs.empty());
  ASSERT_TRUE(configs.back().msaa_samples.has_value());
  EXPECT_EQ(*configs.back().msaa_samples, 0u);
}

// The CLI path is fatal rather than lenient, so this is a death test.
TEST(HomescreenConfigurationMsaaDeathTest, CliRejectsOutOfRangeCount) {
  EXPECT_EXIT(ParseCli({"--msaa-samples", "3"}),
              ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

// ---------------------------------------------------------------------------
// CLI override merge
// ---------------------------------------------------------------------------
//
// The value has to survive get_cli_override, or --msaa-samples would parse and
// then never reach the resolved per-view config. That is exactly the bug the
// RDK parameters hit, so it is pinned here explicitly.

TEST(HomescreenConfigurationMsaa, CliOverridesTomlValue) {
  Configuration::Config from_toml{};
  from_toml.msaa_samples = 8;

  Configuration::Config from_cli{};
  from_cli.msaa_samples = 2;

  Configuration::get_cli_override(from_toml.view.bundle_path, from_toml,
                                  from_cli);

  ASSERT_TRUE(from_toml.msaa_samples.has_value());
  EXPECT_EQ(*from_toml.msaa_samples, 2u);
}

TEST(HomescreenConfigurationMsaa, UnsetCliLeavesTomlValueIntact) {
  Configuration::Config from_toml{};
  from_toml.msaa_samples = 8;

  const Configuration::Config from_cli{};

  Configuration::get_cli_override(from_toml.view.bundle_path, from_toml,
                                  from_cli);

  ASSERT_TRUE(from_toml.msaa_samples.has_value());
  EXPECT_EQ(*from_toml.msaa_samples, 8u);
}

// An explicit --msaa-samples 0 must override a configured non-zero value
// rather than being mistaken for "nothing supplied".
TEST(HomescreenConfigurationMsaa, CliZeroOverridesNonZeroTomlValue) {
  Configuration::Config from_toml{};
  from_toml.msaa_samples = 4;

  Configuration::Config from_cli{};
  from_cli.msaa_samples = 0;

  Configuration::get_cli_override(from_toml.view.bundle_path, from_toml,
                                  from_cli);

  ASSERT_TRUE(from_toml.msaa_samples.has_value());
  EXPECT_EQ(*from_toml.msaa_samples, 0u);
}
