#include <stdlib.h>
#include "gtest/gtest.h"

#include <rapidjson/document.h>
#include <configuration/configuration.h>

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetViewParameters_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of getViewParameters with all normal parameters
***************************************************************/

TEST(HomescreenConfigurationGetViewParameters, Lv1Normal001) {
  struct Configuration::Config config {};
  std::string json_str =
      "{"
      "\"bundle_path\":\"/home\","
      "\"window_type\":\"NORMAL\","
      "\"output_index\":1,"
      "\"width\":1280,"
      "\"height\":720,"
      "\"pixel_ratio\":1,"
      "\"ivi_surface_id\":1,"
      "\"vm_args\":["
          "\"--enable-asserts\", \"--pause-isolates-on-start\""
      "],"
      "\"accessibility_features\":1,"
      "\"fullscreen\":true,"
      "\"debug_backend\":true,"
      "\"fps_output_console\":1,"
      "\"fps_output_overlay\":1,"
      "\"fps_output_frequency\":1"
      "}";

  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  // call target function
  Configuration::getViewParameters(doc.GetObject(), config);

  EXPECT_EQ("/home", config.view.bundle_path);
  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(1, config.view.wl_output_index);
  EXPECT_EQ(1280, config.view.width);
  EXPECT_EQ(720, config.view.height);
  EXPECT_EQ(1, config.view.pixel_ratio);
  EXPECT_EQ(1, config.view.ivi_surface_id);
  EXPECT_EQ("--enable-asserts", config.view.vm_args[0]);
  EXPECT_EQ("--pause-isolates-on-start", config.view.vm_args[1]);
  EXPECT_EQ(1, config.view.accessibility_features);
  EXPECT_EQ(true, config.view.fullscreen);
  EXPECT_EQ(true, config.debug_backend);
  EXPECT_EQ(1, config.view.fps_output_console);
  EXPECT_EQ(1, config.view.fps_output_overlay);
  EXPECT_EQ(1, config.view.fps_output_frequency);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetViewParameters_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test getViewParameters with other normal parameter and check unset parameters
***************************************************************/

TEST(HomescreenConfigurationGetViewParameters, Lv1Normal002) {
  struct Configuration::Config config {};
  std::string json_str =
      "{"
      "\"pixel_ratio\":1.2,"
      "\"vm_args\":[]"
      "}";

  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  // call target function
  Configuration::getViewParameters(doc.GetObject(), config);

  EXPECT_EQ("", config.view.bundle_path);
  EXPECT_EQ("", config.view.window_type);
  EXPECT_EQ(0, config.view.wl_output_index);
  EXPECT_EQ(0, config.view.width);
  EXPECT_EQ(0, config.view.height);
  EXPECT_EQ(1.2, config.view.pixel_ratio);
  EXPECT_EQ(0, config.view.ivi_surface_id);
  EXPECT_EQ(true, config.view.vm_args.empty());
  EXPECT_EQ(0, config.view.accessibility_features);
  EXPECT_EQ(false, config.view.fullscreen);
  EXPECT_EQ(false, config.debug_backend);
  EXPECT_EQ(0, config.view.fps_output_console);
  EXPECT_EQ(0, config.view.fps_output_overlay);
  EXPECT_EQ(0, config.view.fps_output_frequency);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetViewParameters_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test getViewParameters with abnormal parameters
***************************************************************/

TEST(HomescreenConfigurationGetViewParameters, Lv1Abnormal001) {
  struct Configuration::Config config {};
  std::string json_str =
      "{"
      "\"bundle_path\":1,"
      "\"window_type\":1,"
      "\"output_index\":1.234,"
      "\"width\":1.234,"
      "\"height\":1.234,"
      "\"pixel_ratio\":\"Abnormal\","
      "\"ivi_surface_id\":1.234,"
      "\"vm_args\":\"Abnormal\","
      "\"accessibility_features\":1.234,"
      "\"fullscreen\":3,"
      "\"debug_backend\":3,"
      "\"fps_output_console\":1.234,"
      "\"fps_output_overlay\":1.234,"
      "\"fps_output_frequency\":1.234"
      "}";

  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  // call target function
  Configuration::getViewParameters(doc.GetObject(), config);

  EXPECT_EQ("", config.view.bundle_path);
  EXPECT_EQ("", config.view.window_type);
  EXPECT_EQ(0, config.view.wl_output_index);
  EXPECT_EQ(0, config.view.width);
  EXPECT_EQ(0, config.view.height);
  EXPECT_EQ(0, config.view.pixel_ratio);
  EXPECT_EQ(0, config.view.ivi_surface_id);
  EXPECT_EQ(true, config.view.vm_args.empty());
  EXPECT_EQ(0, config.view.accessibility_features);
  EXPECT_EQ(false, config.view.fullscreen);
  EXPECT_EQ(false, config.debug_backend);
  EXPECT_EQ(0, config.view.fps_output_console);
  EXPECT_EQ(0, config.view.fps_output_overlay);
  EXPECT_EQ(0, config.view.fps_output_frequency);
}
