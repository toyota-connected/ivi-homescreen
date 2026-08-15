#include <filesystem>
#include <stdexcept>

#include "gtest/gtest.h"

#include <configuration/configuration.h>
#include <rapidjson/document.h>
#include "unit_test_utils.h"

static constexpr char kSourceRoot[] = SOURCE_ROOT_DIR;
static constexpr char kUnitTestAppBundle[] = UNIT_TEST_APP_BUNDLE;

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationParseConfig_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test ParseConfig for default value
***************************************************************/

TEST(HomescreenConfigurationParseConfig, Lv1Normal001) {
  struct Configuration::Config config{};
  config.bundle_paths.emplace_back(kUnitTestAppBundle);

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::parse_config(config);

  const auto& [app_id, cursor_theme, disable_cursor, wayland_event_mask,
               debug_backend, bundle_paths, enable_mcp, mcp_allowed_tools, view,
               hud, config_file] = configs.back();

  EXPECT_EQ("com.toyotaconnected.homescreen", app_id);
  EXPECT_EQ(false, disable_cursor.value_or(false));
  EXPECT_EQ(false, debug_backend.value_or(false));
  // The MCP surface is opt-in: absent from config means off, never inherited.
  EXPECT_EQ(false, enable_mcp.value_or(false));
  EXPECT_EQ(kBundlePath, view.bundle_path);
  EXPECT_EQ("NORMAL", view.window_type);
  EXPECT_EQ(0, view.wl_output_index.value_or(0));
  EXPECT_EQ(1920, view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(720, view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(false, view.fullscreen.value_or(false));
  EXPECT_EQ(1, view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ(0, view.accessibility_features.value_or(0));
}

/****************************************************************
Test Case Name.Test Name：
HomescreenConfigurationConfigFromArgcArgv_Lv1Normal001 Use Case Name:
Initialization Test Summary：Test the function of ConfigFromArgcArgv
***************************************************************/

TEST(HomescreenConfigurationParseArgcArgv, Lv1Normal001) {
  // ParseArgcArgv validates that the bundle path is an existing directory.
  // kUnitTestAppBundle (/home/root) is only valid on a target device.  Use
  // the test source root (SOURCE_ROOT_DIR) which is guaranteed to exist on
  // every machine that builds the tests.
  constexpr int argc = 26;
  const char* argv[26] = {"homescreen",
                          "-b",
                          kSourceRoot,
                          "-a",
                          "2",
                          "-c",
                          "-d",
                          "-f",
                          "-w",
                          "800",
                          "--height",
                          "600",
                          "-p",
                          "2.0",
                          "-t",
                          "DMZ-White",
                          "--window-type",
                          "NORMAL",
                          "-o",
                          "1",
                          "--xdg-shell-app-id",
                          "homescreen",
                          "--wayland-event-mask",
                          "pointer-axis",
                          "--ivi-surface-id",
                          "1"};
  char** argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  const auto& [app_id, cursor_theme, disable_cursor, wayland_event_mask,
               debug_backend, bundle_paths, enable_mcp, mcp_allowed_tools, view,
               hud, config_file] = configs.back();
  // check result

  EXPECT_EQ(kSourceRoot, view.bundle_path);
  EXPECT_EQ(2, view.accessibility_features.value_or(0));
  EXPECT_EQ(true, disable_cursor.value_or(false));
  EXPECT_EQ(true, debug_backend.value_or(false));
  EXPECT_EQ(true, view.fullscreen.value_or(false));
  EXPECT_EQ(800, view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(600, view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(2, view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ("DMZ-White", cursor_theme);
  EXPECT_EQ("NORMAL", view.window_type);
  EXPECT_EQ(1, view.wl_output_index.value_or(0));
  EXPECT_EQ("homescreen", app_id);
  EXPECT_EQ("pointer-axis", wayland_event_mask);
  EXPECT_EQ("NORMAL", view.window_type);
  EXPECT_EQ(1, view.ivi_surface_id);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationgetView_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test getView with view key
***************************************************************/
TEST(HomescreenConfigurationGetTomlConfig, Lv1Normal001) {
  // set test parameters
  Configuration::Config config{};
  std::filesystem::path config_toml_path = kSourceRoot;
  config_toml_path /= "files/GetTomlConfig_Lv1Normal001.toml";

  // call target function
  Configuration::get_toml_config(config_toml_path.c_str(), config);

  // check result
  EXPECT_EQ("gallery", config.app_id);
  EXPECT_EQ("Coolbeans", config.cursor_theme);
  EXPECT_EQ(true, config.disable_cursor.value_or(false));
  EXPECT_EQ("keyboard", config.wayland_event_mask);
  EXPECT_EQ(false, config.debug_backend.value_or(false));

  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(2, config.view.wl_output_index.value_or(0));
  EXPECT_EQ(1920, config.view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(1080, config.view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(4.5, config.view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ(5002, config.view.ivi_surface_id);
  EXPECT_EQ(52, config.view.accessibility_features.value_or(0));
  EXPECT_EQ(false, config.view.fullscreen.value_or(false));
  EXPECT_EQ(2, config.view.engine_args.size());
  EXPECT_EQ("--enable-asserts", config.view.engine_args[0]);
  EXPECT_EQ("--verbose-logging", config.view.engine_args[1]);
  EXPECT_EQ(10, config.view.activation_area_x);
  EXPECT_EQ(10, config.view.activation_area_y);
  EXPECT_EQ(1024, config.view.activation_area_width);
  EXPECT_EQ(768, config.view.activation_area_height);
}
/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationgetView_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test getView without view param
***************************************************************/

TEST(HomescreenConfigurationGetTomlConfig, Lv1Normal002) {
  // set test parameters
  Configuration::Config config{};
  std::filesystem::path config_toml_path = kSourceRoot;
  config_toml_path /= "files/GetTomlConfig_Lv1Normal002.toml";

  // call target function
  Configuration::get_toml_config(config_toml_path.c_str(), config);

  // check result
  EXPECT_EQ("gallery", config.app_id);
  EXPECT_EQ("Coolbeans", config.cursor_theme);
  EXPECT_EQ(true, config.disable_cursor.value_or(false));
  EXPECT_EQ("keyboard", config.wayland_event_mask);
  EXPECT_EQ(false, config.debug_backend.value_or(false));

  EXPECT_EQ("", config.view.bundle_path);
  EXPECT_EQ("", config.view.window_type);
  EXPECT_EQ(0, config.view.wl_output_index.value_or(0));
  EXPECT_EQ(kDefaultViewWidth, config.view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(kDefaultViewHeight,
            config.view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(kDefaultPixelRatio,
            config.view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ(0, config.view.ivi_surface_id.value_or(0));
  EXPECT_EQ(0, config.view.accessibility_features.value_or(0));
  EXPECT_EQ(false, config.view.fullscreen.value_or(false));
  EXPECT_EQ(0, config.view.engine_args.size());
  EXPECT_EQ(0, config.view.activation_area_x);
  EXPECT_EQ(0, config.view.activation_area_y);
  EXPECT_EQ(0, config.view.activation_area_width);
  EXPECT_EQ(0, config.view.activation_area_height);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetTomlConfig_Backend
Use Case Name: Initialization
Test Summary：[view.backend] / [view.backend.drm] nesting maps to view.drm_*
***************************************************************/
TEST(HomescreenConfigurationGetTomlConfig, Backend) {
  Configuration::Config config{};
  std::filesystem::path config_toml_path = kSourceRoot;
  config_toml_path /= "files/GetTomlConfig_Backend.toml";

  Configuration::get_toml_config(config_toml_path.c_str(), config);

  EXPECT_EQ("drm-kms-vulkan", config.view.backend.value_or(""));
  EXPECT_EQ("/dev/dri/card0", config.view.drm_device.value_or(""));
  EXPECT_EQ("eDP-1", config.view.drm_connector.value_or(""));
  EXPECT_EQ("1920x1080@60", config.view.drm_mode.value_or(""));
  EXPECT_EQ(90, config.view.drm_rotation.value_or(0));
  EXPECT_EQ(true, config.view.drm_no_seat.value_or(false));
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetTomlConfig_Output
Use Case Name: Initialization
Test Summary：[view.output] — the `name` convenience fills both the wl_output
and DRM connector fields; serial / preload / on_disconnect parse.
***************************************************************/
TEST(HomescreenConfigurationGetTomlConfig, Output) {
  Configuration::Config config{};
  std::filesystem::path config_toml_path = kSourceRoot;
  config_toml_path /= "files/GetTomlConfig_Output.toml";

  Configuration::get_toml_config(config_toml_path.c_str(), config);

  // `name` fills both backend name fields.
  EXPECT_EQ("DP-2", config.view.output.wl_name.value_or(""));
  EXPECT_EQ("DP-2", config.view.output.drm_connector.value_or(""));
  EXPECT_EQ("SN-1234", config.view.output.edid_serial.value_or(""));
  EXPECT_TRUE(config.view.output.preload);
  EXPECT_EQ(homescreen::OutputMatch::OnDisconnect::kTeardown,
            config.view.output.on_disconnect);
  EXPECT_FALSE(config.view.output.empty());
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetTomlConfig_OutputArray
Use Case Name: Initialization
Test Summary：A [[view.output]] array binds the first entry as the primary and
the rest as additional_outputs. Applying the same file twice to one Config (as
a bundle config.toml layered under a master --config) must replace the extras,
not concatenate them.
***************************************************************/
TEST(HomescreenConfigurationGetTomlConfig, OutputArray) {
  Configuration::Config config{};
  std::filesystem::path config_toml_path = kSourceRoot;
  config_toml_path /= "files/GetTomlConfig_OutputArray.toml";

  Configuration::get_toml_config(config_toml_path.c_str(), config);

  // Primary = first entry; one extra = second entry.
  EXPECT_EQ("DP-1", config.view.output.wl_name.value_or(""));
  ASSERT_EQ(1u, config.view.additional_outputs.size());
  EXPECT_EQ("HDMI-1",
            config.view.additional_outputs.front().wl_name.value_or(""));

  // Layering a second time (a later config layer) replaces the extras rather
  // than appending — the count stays 1, it does not grow to 2.
  Configuration::get_toml_config(config_toml_path.c_str(), config);
  ASSERT_EQ(1u, config.view.additional_outputs.size());
  EXPECT_EQ("HDMI-1",
            config.view.additional_outputs.front().wl_name.value_or(""));
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationParseConfig_MultiView
Use Case Name: Initialization
Test Summary：A --config master file yields one Config per [[view]], each with
its own activation area (proves per-view, not global) and full-extent default.
***************************************************************/
TEST(HomescreenConfigurationParseConfig, MultiView) {
  Configuration::Config config{};
  std::filesystem::path master = kSourceRoot;
  master /= "files/ParseConfig_MultiView.toml";
  config.config_file = master.string();

  const auto configs = Configuration::parse_config(config);
  ASSERT_EQ(2u, configs.size());

  // view A: explicit activation area.
  EXPECT_EQ("cluster-suite", configs[0].app_id);
  EXPECT_EQ("agl", configs[0].view.shell.value_or(""));
  EXPECT_EQ("BG", configs[0].view.window_type);
  EXPECT_EQ(0, configs[0].view.activation_area_x);
  EXPECT_EQ(60, configs[0].view.activation_area_y);
  EXPECT_EQ(1920, configs[0].view.activation_area_width);
  EXPECT_EQ(1020, configs[0].view.activation_area_height);

  // view B: omitted activation area -> full view extent (1280x720).
  EXPECT_EQ("cluster-suite", configs[1].app_id);
  EXPECT_EQ(0, configs[1].view.activation_area_x);
  EXPECT_EQ(0, configs[1].view.activation_area_y);
  EXPECT_EQ(1280, configs[1].view.activation_area_width);
  EXPECT_EQ(720, configs[1].view.activation_area_height);

  // The whole point: per-view areas differ rather than sharing one global rect.
  EXPECT_NE(configs[0].view.activation_area_height,
            configs[1].view.activation_area_height);

  // Bundle paths resolve relative to the master file's directory.
  EXPECT_NE(std::string::npos, configs[0].view.bundle_path.find("viewA"));
  EXPECT_NE(std::string::npos, configs[1].view.bundle_path.find("viewB"));
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationParseConfig_LayeredMerge
Use Case Name: Initialization
Test Summary：--config view entry overrides the bundle's own config per-key,
while unset keys fall through to the bundle config.
***************************************************************/
TEST(HomescreenConfigurationParseConfig, LayeredMerge) {
  Configuration::Config config{};
  std::filesystem::path master = kSourceRoot;
  master /= "files/Merge.toml";
  config.config_file = master.string();

  const auto configs = Configuration::parse_config(config);
  ASSERT_EQ(1u, configs.size());
  const auto& v = configs[0];

  EXPECT_EQ("from-master", v.app_id);          // master [global] overrides
  EXPECT_EQ(800, v.view.width.value_or(0));    // fell through from bundle
  EXPECT_EQ(1200, v.view.height.value_or(0));  // overridden by master view
  EXPECT_EQ("BG", v.view.window_type);         // overridden by master view
  EXPECT_EQ(1, v.view.activation_area_x);      // fell through from bundle
  EXPECT_EQ(2, v.view.activation_area_y);
  EXPECT_EQ(3, v.view.activation_area_width);
  EXPECT_EQ(4, v.view.activation_area_height);
}
