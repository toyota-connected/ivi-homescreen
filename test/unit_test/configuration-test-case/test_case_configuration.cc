#include <filesystem>
#include <stdexcept>

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

#include <configuration/configuration.h>
#include <rapidjson/document.h>
#include "unit_test_utils.h"

static constexpr char kSourceRoot[] = SOURCE_ROOT_DIR;

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationParseConfig_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test ParseConfig for default value
***************************************************************/

TEST(HomescreenConfigurationParseConfig, Lv1Normal001) {
  struct Configuration::Config config {};
  config.bundle_paths.push_back("/home/root/");

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::parse_config(config);

  Configuration::Config config_ret = configs.back();

  EXPECT_EQ("homescreen", config_ret.app_id);
  EXPECT_EQ(false, config_ret.disable_cursor.value_or(false));
  EXPECT_EQ(false, config_ret.debug_backend.value_or(false));
  EXPECT_EQ("/home/root/", config_ret.view.bundle_path);
  EXPECT_EQ("NORMAL", config_ret.view.window_type);
  EXPECT_EQ(0, config_ret.view.wl_output_index.value_or(0));
  EXPECT_EQ(1920, config_ret.view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(720, config_ret.view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(false, config_ret.view.fullscreen.value_or(false));
  EXPECT_EQ(1, config_ret.view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ(0, config_ret.view.accessibility_features.value_or(0));
}

/****************************************************************
Test Case Name.Test Name：
HomescreenConfigurationConfigFromArgcArgv_Lv1Normal001 Use Case Name:
Initialization Test Summary：Test the function of ConfigFromArgcArgv
***************************************************************/

TEST(HomescreenConfigurationParseArgcArgv, Lv1Normal001) {
  // setup test parameters
  int argc = 29;
  const char* argv[29] = {"homescreen",
                         "-b","/home/root",
                         "-a","2",
                         "-c","true",
                         "-d","true",
                         "-f","true",
                         "-w","800",
                         "-h","600",
                         "-p","2.0f",
                         "-t","DMZ-White",
                         "--window-type","NORMAL",
                         "-o","1",
                         "--xdg-shell-app-id","homescreen",
                         "--event-mask", "pointer-axis",
                         "--ivi-surface-id", "1"};
  char** argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  Configuration::Config config_ret = configs.back();
  // check result

  EXPECT_EQ("/home/root", config_ret.view.bundle_path);
  EXPECT_EQ(2, config_ret.view.accessibility_features.value_or(0));
  EXPECT_EQ(true, config_ret.disable_cursor.value_or(false));
  EXPECT_EQ(true, config_ret.debug_backend.value_or(false));
  EXPECT_EQ(true, config_ret.view.fullscreen.value_or(false));
  EXPECT_EQ(800, config_ret.view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(600, config_ret.view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(2, config_ret.view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ("DMZ-White", config_ret.cursor_theme);
  EXPECT_EQ("NORMAL", config_ret.view.window_type);
  EXPECT_EQ(1, config_ret.view.wl_output_index.value_or(0));
  EXPECT_EQ("homescreen", config_ret.app_id);
  EXPECT_EQ("pointer-axis", config_ret.wayland_event_mask);
  EXPECT_EQ("NORMAL", config_ret.view.window_type);
  EXPECT_EQ(1, config_ret.view.ivi_surface_id);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationgetView_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test getView with view key
***************************************************************/
TEST(HomescreenConfigurationGetTomlConfig, Lv1Normal001) {
  // set test parameters
  struct Configuration::Config config {};
  const char* config_toml_path = "files/GetTomlConfig_Lv1Normal001.toml";

  // call target function
  Configuration::get_toml_config(config_toml_path, config);

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
  EXPECT_EQ(2, config.view.vm_args.size());
  EXPECT_EQ("--enable-asserts", config.view.vm_args[0]);
  EXPECT_EQ("--verbose-logging", config.view.vm_args[1]);
  EXPECT_EQ(1, config.view.fps_output_console);
  EXPECT_EQ(1, config.view.fps_output_overlay);
  EXPECT_EQ(3, config.view.fps_output_frequency);
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
  struct Configuration::Config config {};
  const char* config_toml_path = "files/GetTomlConfig_Lv1Normal002.toml";

  // call target function
  Configuration::get_toml_config(config_toml_path, config);

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
  EXPECT_EQ(kDefaultViewHeight, config.view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(kDefaultPixelRatio, config.view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ(0, config.view.ivi_surface_id.value_or(0));
  EXPECT_EQ(0, config.view.accessibility_features.value_or(0));
  EXPECT_EQ(false, config.view.fullscreen.value_or(false));
  EXPECT_EQ(0, config.view.vm_args.size());
  EXPECT_EQ(0, config.view.fps_output_console);
  EXPECT_EQ(0, config.view.fps_output_overlay);
  EXPECT_EQ(0, config.view.fps_output_frequency);
  EXPECT_EQ(0, config.view.activation_area_x);
  EXPECT_EQ(0, config.view.activation_area_y);
  EXPECT_EQ(0, config.view.activation_area_width);
  EXPECT_EQ(0, config.view.activation_area_height);
}

