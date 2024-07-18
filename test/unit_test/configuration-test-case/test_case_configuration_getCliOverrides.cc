#include <stdlib.h>
#include "gtest/gtest.h"

#include <flutter/fml/command_line.h>
#include <rapidjson/document.h>
#include <configuration/configuration.h>

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetCliOverrides_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test getCliOverrides with all normal parameters
***************************************************************/

TEST(HomescreenConfigurationGetCliOverrides, Lv1Normal001) {
  // set test parameters
  struct Configuration::Config input_cfg {};
  struct Configuration::Config config {};

  input_cfg.app_id = "homescreen";
  input_cfg.bundle_paths.emplace_back("files");
  input_cfg.cursor_theme = "DMZ-White";
  input_cfg.disable_cursor = true;
  input_cfg.wayland_event_mask = "keyboard";
  input_cfg.debug_backend = true;
  input_cfg.view.vm_args.push_back("--enable-asserts");
  input_cfg.view.vm_args.push_back("--pause-isolates-on-start");
  input_cfg.view.bundle_path = "files";
  input_cfg.view.window_type = "NORMAL";
  input_cfg.view.wl_output_index = 1;
  input_cfg.view.accessibility_features = 1;
  input_cfg.view.width = 1280;
  input_cfg.view.height = 720;
  input_cfg.view.pixel_ratio = 1;
  input_cfg.view.ivi_surface_id = 1;
  input_cfg.view.fullscreen = true;

  // call target function
  Configuration::get_cli_override(config.view.bundle_path, config, input_cfg);

  // check result
  EXPECT_EQ("homescreen", config.app_id);
  EXPECT_EQ("DMZ-White", config.cursor_theme);
  EXPECT_EQ(true, config.disable_cursor);
  EXPECT_EQ(true, config.debug_backend);
  EXPECT_EQ("--enable-asserts", config.view.vm_args[0]);
  EXPECT_EQ("--pause-isolates-on-start", config.view.vm_args[1]);
  EXPECT_EQ("keyboard", config.wayland_event_mask);

  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(1, config.view.wl_output_index);
  EXPECT_EQ(1, config.view.accessibility_features);
  EXPECT_EQ(1280, config.view.width);
  EXPECT_EQ(720, config.view.height);
  EXPECT_EQ(1, config.view.pixel_ratio);
  EXPECT_EQ(1, config.view.ivi_surface_id);
  EXPECT_EQ(true, config.view.fullscreen);
  EXPECT_EQ(1,1);
}
/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetCliOverrides_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test getCliOverrides with blank parameter
***************************************************************/

TEST(HomescreenConfigurationGetCliOverrides, Lv1Normal002) {
  // set test parameters
  struct Configuration::Config input_cfg {};
  struct Configuration::Config config {};

  // call target function
  Configuration::get_cli_override("/home", config, input_cfg);

  // check result
  EXPECT_EQ("", config.app_id);
  EXPECT_EQ(0, config.bundle_paths.size());
  EXPECT_EQ("", config.cursor_theme);
  EXPECT_EQ(false, config.disable_cursor.value_or(false));
  EXPECT_EQ(false, config.debug_backend.value_or(false));
  EXPECT_EQ(true, config.view.vm_args.empty());
  EXPECT_EQ("", config.wayland_event_mask);

  EXPECT_EQ("/home", config.view.bundle_path);
  EXPECT_EQ("", config.view.window_type);
  EXPECT_EQ(0, config.view.wl_output_index.value_or(0));
  EXPECT_EQ(kDefaultViewWidth, config.view.width.value_or(kDefaultViewWidth));
  EXPECT_EQ(kDefaultViewHeight, config.view.height.value_or(kDefaultViewHeight));
  EXPECT_EQ(kDefaultPixelRatio, config.view.pixel_ratio.value_or(kDefaultPixelRatio));
  EXPECT_EQ(0, config.view.ivi_surface_id.value_or(0));
  EXPECT_EQ(0, config.view.accessibility_features.value_or(0));
  EXPECT_EQ(false, config.view.fullscreen.value_or(false));
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetCliOverrides_Lv1Normal003
Use Case Name: Initialization
Test Summary：Test getCliOverrides with some params already true
***************************************************************/

TEST(HomescreenConfigurationGetCliOverrides, Lv1Normal003) {
  // set test parameters
  struct Configuration::Config input_cfg {};
  struct Configuration::Config config {};

  input_cfg.app_id = "homescreen";
  input_cfg.disable_cursor = true;
  input_cfg.debug_backend = true;
  input_cfg.view.fullscreen = true;

  // set same value with input_cfg
  config.disable_cursor = true;
  config.debug_backend = true;
  config.view.fullscreen = true;

  // call target function
  Configuration::get_cli_override("/home", config, input_cfg);

  // check result
  EXPECT_EQ("homescreen", config.app_id);
  EXPECT_EQ(true, config.disable_cursor);
  EXPECT_EQ(true, config.debug_backend);
  EXPECT_EQ(true, config.view.fullscreen);
}
