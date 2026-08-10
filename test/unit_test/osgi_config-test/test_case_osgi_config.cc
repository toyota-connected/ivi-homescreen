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

#include "osgi/cpu_affinity.h"
#include "osgi/osgi_config.h"

namespace {

// Parse a TOML document from a literal. Keeps each case readable as the schema
// it is testing rather than as a file fixture.
bool ParseText(const std::string& text, ihs::osgi::OsgiConfig& out) {
  auto result = toml::parse(text);
  EXPECT_TRUE(result) << "fixture itself failed to parse";
  if (!result) {
    return false;
  }
  return ihs::osgi::ParseOsgiTable(result.table(), out);
}

// One bundle whose only interesting key is cpu_core.
std::string BundleWithCpuCore(const int core) {
  return
      R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
cpu_core = )" +
      std::to_string(core) + "\n";
}

}  // namespace

// A document with no [osgi] table must succeed and yield nothing. This is the
// ENABLE_OSGI=ON, stock-config case: it has to behave like an OSGi-free build.
TEST(OsgiConfig, NoOsgiTableIsNotAnError) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_TRUE(ParseText("[global]\napp_id = 'homescreen'\n", cfg));
  EXPECT_TRUE(cfg.empty());
  EXPECT_EQ(cfg.framework_core, -1);
}

TEST(OsgiConfig, ParsesFrameworkCoreAndBundles) {
  // Both core values are validated against the affinity mask, so pick one the
  // mask actually contains rather than assuming core 0 or 1 is usable.
  int core = -1;
  for (int cpu = 0; cpu < 1024 && core < 0; ++cpu) {
    if (ihs::osgi::IsCpuAvailable(cpu)) {
      core = cpu;
    }
  }
  ASSERT_GE(core, 0) << "no CPU available to pin to";
  const std::string core_text = std::to_string(core);

  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ParseText(R"(
[osgi]
framework_core = )" + core_text +
                            R"(

[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/usr/share/ivi/bundles/cluster"
priority = "critical"
cpu_core = )" + core_text +
                            R"(
startup_timeout_ms = 750

[[osgi.bundles]]
symbolic_name = "com.ivi.navigation"
bundle = "/usr/share/ivi/bundles/navigation"
)",
                        cfg));

  EXPECT_EQ(cfg.framework_core, core);
  ASSERT_EQ(cfg.bundles.size(), 2u);

  EXPECT_EQ(cfg.bundles[0].symbolic_name, "com.ivi.cluster");
  EXPECT_EQ(cfg.bundles[0].config.view.bundle_path,
            "/usr/share/ivi/bundles/cluster");
  EXPECT_EQ(cfg.bundles[0].priority, ihs::osgi::Priority::kCritical);
  EXPECT_EQ(cfg.bundles[0].cpu_core, core);
  EXPECT_EQ(cfg.bundles[0].startup_timeout_ms, 750);

  // Defaults apply to anything the second entry left unset.
  EXPECT_EQ(cfg.bundles[1].priority, ihs::osgi::Priority::kNormal);
  EXPECT_EQ(cfg.bundles[1].cpu_core, -1);
  EXPECT_EQ(cfg.bundles[1].startup_timeout_ms, 500);
}

// The point of delegating to Configuration::get_view_parameters: a bundle gets
// the full [view] key surface, with [view] semantics, for free.
TEST(OsgiConfig, DelegatesViewKeysToConfigurationParser) {
  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
width = 1280
height = 720

  [osgi.bundles.backend]
  type = "drm-kms-egl"

    [osgi.bundles.backend.drm]
    connector = "eDP-1"
    mode = "1280x720@60"

  [osgi.bundles.args]
  engine = ["--old_gen_heap_size=32"]

  [osgi.bundles.output]
  name = "eDP-1"
)",
                        cfg));

  ASSERT_EQ(cfg.bundles.size(), 1u);
  const auto& view = cfg.bundles[0].config.view;
  EXPECT_EQ(view.width, 1280u);
  EXPECT_EQ(view.height, 720u);
  ASSERT_TRUE(view.backend.has_value());
  EXPECT_EQ(*view.backend, "drm-kms-egl");
  ASSERT_TRUE(view.drm_connector.has_value());
  EXPECT_EQ(*view.drm_connector, "eDP-1");
  ASSERT_TRUE(view.drm_mode.has_value());
  EXPECT_EQ(*view.drm_mode, "1280x720@60");
  ASSERT_EQ(view.engine_args.size(), 1u);
  EXPECT_EQ(view.engine_args[0], "--old_gen_heap_size=32");
  // [view.output] name is the cross-backend convenience: it fills both the
  // Wayland and DRM name fields so one bundle config works on either backend.
  ASSERT_TRUE(view.output.wl_name.has_value());
  EXPECT_EQ(*view.output.wl_name, "eDP-1");
  ASSERT_TRUE(view.output.drm_connector.has_value());
  EXPECT_EQ(*view.output.drm_connector, "eDP-1");
}

// [osgi.bundles.shell] reaches ivi_surface_id through the same path
// [view.shell] does -- no separate OSGi-side schema for it.
TEST(OsgiConfig, ParsesIviShellSurfaceId) {
  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"

  [osgi.bundles.shell]
  type = "ivi"
  surface_id = 1001
)",
                        cfg));

  ASSERT_EQ(cfg.bundles.size(), 1u);
  EXPECT_EQ(cfg.bundles[0].config.view.shell, "ivi");
  ASSERT_TRUE(cfg.bundles[0].config.view.ivi_surface_id.has_value());
  EXPECT_EQ(*cfg.bundles[0].config.view.ivi_surface_id, 1001u);
}

TEST(OsgiConfig, RejectsMissingSymbolicName) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
bundle = "/bundles/cluster"
)",
                         cfg));
}

TEST(OsgiConfig, RejectsMissingBundlePath) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
)",
                         cfg));
}

TEST(OsgiConfig, RejectsUnknownPriority) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
priority = "urgent"
)",
                         cfg));
}

TEST(OsgiConfig, RejectsDuplicateSymbolicName) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/a"

[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/b"
)",
                         cfg));
}

// Two bundles sharing an ivi-shell surface_id would silently collide on the
// compositor. Fail at parse time instead.
TEST(OsgiConfig, RejectsDuplicateIviSurfaceId) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/a"

  [osgi.bundles.shell]
  surface_id = 7

[[osgi.bundles]]
symbolic_name = "com.ivi.navigation"
bundle = "/bundles/b"

  [osgi.bundles.shell]
  surface_id = 7
)",
                         cfg));
}

// Distinct ids are the normal multi-bundle ivi-shell case and must pass.
TEST(OsgiConfig, AcceptsDistinctIviSurfaceIds) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_TRUE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/a"

  [osgi.bundles.shell]
  surface_id = 7

[[osgi.bundles]]
symbolic_name = "com.ivi.navigation"
bundle = "/bundles/b"

  [osgi.bundles.shell]
  surface_id = 8
)",
                        cfg));
  EXPECT_EQ(cfg.bundles.size(), 2u);
}

TEST(OsgiConfig, RejectsNonPositiveStartupTimeout) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
startup_timeout_ms = 0
)",
                         cfg));
}

// A TOML integer is int64_t. These do not fit in int, and must be diagnosed as
// bad config rather than crashing the shell on a narrowing conversion.
TEST(OsgiConfig, RejectsOutOfRangeIntegersWithoutCrashing) {
  for (const char* doc : {R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
cpu_core = 99999999999
)",
                          R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
startup_timeout_ms = 99999999999
)",
                          "[osgi]\nframework_core = 99999999999\n"}) {
    ihs::osgi::OsgiConfig cfg;
    EXPECT_FALSE(ParseText(doc, cfg)) << doc;
  }
}

TEST(OsgiConfig, RejectsNegativeCpuCoreBelowUnpinned) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "/bundles/cluster"
cpu_core = -2
)",
                         cfg));
}

// cpu_core is validated against the process affinity mask, so the values these
// cases use are derived from that mask at run time. Hardcoding a core number
// would make the test pass or fail according to what the CI runner was pinned
// to.
TEST(OsgiConfig, AcceptsAvailableCpuCore) {
  if (!ihs::osgi::CpuAffinityKnown()) {
    GTEST_SKIP() << "process affinity mask unavailable";
  }
  int available = -1;
  for (int cpu = 0; cpu < 1024; ++cpu) {
    if (ihs::osgi::IsCpuAvailable(cpu)) {
      available = cpu;
      break;
    }
  }
  ASSERT_GE(available, 0) << "no CPU available to pin to";

  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ParseText(BundleWithCpuCore(available), cfg));
  ASSERT_EQ(cfg.bundles.size(), 1u);
  EXPECT_EQ(cfg.bundles[0].cpu_core, available);
}

// The case a plain core *count* check would wrongly admit: an index inside
// [0, count) that the affinity mask nonetheless excludes, or one past the top
// of the mask. Either way pthread_setaffinity_np would refuse it later.
TEST(OsgiConfig, RejectsCpuCoreOutsideAffinityMask) {
  if (!ihs::osgi::CpuAffinityKnown()) {
    GTEST_SKIP() << "process affinity mask unavailable";
  }
  const std::optional<int> highest = ihs::osgi::HighestAvailableCpu();
  ASSERT_TRUE(highest.has_value());

  // First index above the mask that is still a legal CPU_SETSIZE index.
  int unavailable = -1;
  for (int cpu = *highest + 1; cpu < 1024; ++cpu) {
    if (!ihs::osgi::IsCpuAvailable(cpu)) {
      unavailable = cpu;
      break;
    }
  }
  if (unavailable < 0) {
    GTEST_SKIP() << "every CPU index is available; nothing to reject";
  }

  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(BundleWithCpuCore(unavailable), cfg));
}

// The precise case that separates a mask check from a core-count check: an
// index *below* the highest available one that the mask still excludes. Under
// `taskset -c 2,3` that is index 0 or 1 -- both inside [0, count) for a count
// of 2, and both refused by pthread_setaffinity_np. Skipped when the mask
// happens to be contiguous from 0, where the two checks agree.
TEST(OsgiConfig, RejectsExcludedCpuBelowHighestAvailable) {
  if (!ihs::osgi::CpuAffinityKnown()) {
    GTEST_SKIP() << "process affinity mask unavailable";
  }
  const std::optional<int> highest = ihs::osgi::HighestAvailableCpu();
  ASSERT_TRUE(highest.has_value());

  int excluded = -1;
  for (int cpu = 0; cpu < *highest; ++cpu) {
    if (!ihs::osgi::IsCpuAvailable(cpu)) {
      excluded = cpu;
      break;
    }
  }
  if (excluded < 0) {
    GTEST_SKIP() << "affinity mask is contiguous from 0; no excluded index "
                    "below the top to test";
  }

  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(BundleWithCpuCore(excluded), cfg))
      << "cpu " << excluded << " is outside the mask but was accepted";
}

TEST(OsgiConfig, RejectsFrameworkCoreOutsideAffinityMask) {
  if (!ihs::osgi::CpuAffinityKnown()) {
    GTEST_SKIP() << "process affinity mask unavailable";
  }
  const std::optional<int> highest = ihs::osgi::HighestAvailableCpu();
  ASSERT_TRUE(highest.has_value());
  int unavailable = -1;
  for (int cpu = *highest + 1; cpu < 1024; ++cpu) {
    if (!ihs::osgi::IsCpuAvailable(cpu)) {
      unavailable = cpu;
      break;
    }
  }
  if (unavailable < 0) {
    GTEST_SKIP() << "every CPU index is available; nothing to reject";
  }

  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText(
      "[osgi]\nframework_core = " + std::to_string(unavailable) + "\n", cfg));
}

// -1 means "do not pin" and must be accepted whatever the mask looks like.
TEST(OsgiConfig, AcceptsUnpinnedSentinel) {
  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ParseText(BundleWithCpuCore(-1), cfg));
  ASSERT_EQ(cfg.bundles.size(), 1u);
  EXPECT_EQ(cfg.bundles[0].cpu_core, -1);
}

// A relative 'bundle' must resolve against the config file's directory, the
// same way Configuration::parse_config resolves it for [[view]]. Otherwise one
// path spelling means two different things within a single file.
TEST(OsgiConfig, ResolvesRelativeBundlePathAgainstBaseDir) {
  ihs::osgi::OsgiConfig cfg;
  auto result = toml::parse(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "cluster"

[[osgi.bundles]]
symbolic_name = "com.ivi.navigation"
bundle = "/absolute/navigation"
)");
  ASSERT_TRUE(result);
  ASSERT_TRUE(ihs::osgi::ParseOsgiTable(result.table(), cfg, "/etc/ivi"));

  ASSERT_EQ(cfg.bundles.size(), 2u);
  EXPECT_EQ(cfg.bundles[0].config.view.bundle_path, "/etc/ivi/cluster");
  // An absolute path is never rewritten.
  EXPECT_EQ(cfg.bundles[1].config.view.bundle_path, "/absolute/navigation");
}

// With no base dir (an in-memory document), a relative path stays as written.
TEST(OsgiConfig, LeavesRelativeBundlePathWhenNoBaseDir) {
  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ParseText(R"(
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "cluster"
)",
                        cfg));
  ASSERT_EQ(cfg.bundles.size(), 1u);
  EXPECT_EQ(cfg.bundles[0].config.view.bundle_path, "cluster");
}

TEST(OsgiConfig, RejectsBundlesAsNonArray) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ParseText("[osgi]\nbundles = 3\n", cfg));
}

TEST(OsgiConfig, PriorityNamesRoundTrip) {
  for (const auto priority :
       {ihs::osgi::Priority::kCritical, ihs::osgi::Priority::kNormal,
        ihs::osgi::Priority::kBackground}) {
    ihs::osgi::Priority parsed{};
    ASSERT_TRUE(
        ihs::osgi::ParsePriority(ihs::osgi::PriorityName(priority), parsed));
    EXPECT_EQ(parsed, priority);
  }
}

TEST(OsgiConfig, LoadOsgiConfigReportsMissingFile) {
  ihs::osgi::OsgiConfig cfg;
  EXPECT_FALSE(ihs::osgi::LoadOsgiConfig(
      std::string(SOURCE_ROOT_DIR) + "/files/does_not_exist.toml", cfg));
}

TEST(OsgiConfig, LoadOsgiConfigReadsFromDisk) {
  ihs::osgi::OsgiConfig cfg;
  ASSERT_TRUE(ihs::osgi::LoadOsgiConfig(
      std::string(SOURCE_ROOT_DIR) + "/files/osgi_two_bundles.toml", cfg));
  // The fixture deliberately sets no framework_core, so it keeps the unpinned
  // default; pinning is covered by the affinity-mask cases above.
  EXPECT_EQ(cfg.framework_core, -1);
  ASSERT_EQ(cfg.bundles.size(), 2u);
  EXPECT_EQ(cfg.bundles[0].symbolic_name, "com.ivi.cluster");
  EXPECT_EQ(cfg.bundles[0].priority, ihs::osgi::Priority::kCritical);
  EXPECT_EQ(cfg.bundles[1].symbolic_name, "com.ivi.navigation");
  EXPECT_EQ(cfg.bundles[1].priority, ihs::osgi::Priority::kNormal);
}
