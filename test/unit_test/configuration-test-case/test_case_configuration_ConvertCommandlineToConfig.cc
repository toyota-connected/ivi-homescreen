#include <stdlib.h>
#include "gtest/gtest.h"

#include <flutter/fml/command_line.h>
#include <rapidjson/document.h>
#include <configuration/configuration.h>

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test ConvertCommandlineToConfig with normal parameters
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Normal001) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--j=/tmp/cfg-dbg.json",
                         "--a=31",
                         "--b=/home",
                         "--c",
                         "--d",
                         "--f",
                         "--w=1280",
                         "--h=720",
                         "--t=DMZ-White",
                         "--window-type=NORMAL",
                         "--output-index=1",
                         "--xdg-shell-app-id=homescreen",
                         "--wayland-event-mask=touch",
                         "--p=1",
                         "--i=1"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(0, ret_convert);

  EXPECT_EQ("/tmp/cfg-dbg.json", config.json_configuration_path);
  EXPECT_EQ(31, config.view.accessibility_features);
  EXPECT_EQ("/home", config.view.bundle_path);
  EXPECT_EQ(true, config.disable_cursor);
  EXPECT_EQ(true, config.disable_cursor_set);
  EXPECT_EQ(true, config.debug_backend);
  EXPECT_EQ(true, config.debug_backend_set);

  EXPECT_EQ(true, config.view.fullscreen);
  EXPECT_EQ(1280, config.view.width);
  EXPECT_EQ(720, config.view.height);
  EXPECT_EQ("DMZ-White", config.cursor_theme);
  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(1, config.view.wl_output_index);
  EXPECT_EQ("homescreen", config.app_id);
  EXPECT_EQ("touch", config.wayland_event_mask);
  EXPECT_EQ(1, config.view.pixel_ratio);
  EXPECT_EQ(1, config.view.ivi_surface_id);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test with blank all parameters
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Normal002) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(0, ret_convert);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test ConvertCommandlineToConfig with blank json param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal001) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--j="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);

  EXPECT_EQ("", config.json_configuration_path);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal002
Use Case Name: Initialization
Test Summary：Test with blank accessibility_features param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal002) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--a="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal003
Use Case Name: Initialization
Test Summary：Test with float accessibility_features param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal003) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--a=invalid-value"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal004
Use Case Name: Initialization
Test Summary：Test with accessibility_features param out of range
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal004) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--a=9223372036854775808"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal005
Use Case Name: Initialization
Test Summary：Test with blank bundle-path param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal005) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--b="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ("", config.view.bundle_path);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal006
Use Case Name: Initialization
Test Summary：Test with blank width param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal006) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--w="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.width);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal007
Use Case Name: Initialization
Test Summary：Test with non-value width param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal007) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--w=abnormal"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.width);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal008
Use Case Name: Initialization
Test Summary：Test with blank height param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal008) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--h="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.height);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal009
Use Case Name: Initialization
Test Summary：Test with non-value height param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal009) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--h=abnormal"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.height);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal010
Use Case Name: Initialization
Test Summary：Test with blank cursol_theme param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal010) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--t="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ("", config.cursor_theme);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal011
Use Case Name: Initialization
Test Summary：Test with blank output_index param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal011) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--output-index="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.wl_output_index);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal012
Use Case Name: Initialization
Test Summary：Test with non-value output_index param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal012) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--output-index=abnormal"};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.wl_output_index);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal013
Use Case Name: Initialization
Test Summary：Test with blank xdg-shell-app-id param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal013) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--xdg-shell-app-id="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ("", config.app_id);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal014
Use Case Name: Initialization
Test Summary：Test with blank wayland_event_mask param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal014) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--wayland-event-mask="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ("", config.wayland_event_mask);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal015
Use Case Name: Initialization
Test Summary：Test with blank pixel_ratio param
***************************************************************/
TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal015) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--p="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.pixel_ratio);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal016
Use Case Name: Initialization
Test Summary：Test with blank ivi_surface_id param
***************************************************************/
TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal016) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--i="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ(0, config.view.ivi_surface_id);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationConvertCommand_Lv1Abnormal017
Use Case Name: Initialization
Test Summary：Test with blank window_type param
***************************************************************/

TEST(HomescreenConfigurationConvertCommand, Lv1Abnormal017) {
  // setup test parameters
  struct Configuration::Config config {};
  const char* argv[] =  {"homescreen",
                         "--window-type="};
  char** argv_p = (char**)&argv;
  int argc = sizeof(argv) / sizeof(argv[0]);

  auto cl = fml::CommandLineFromArgcArgv(argc, argv_p);

  // call target function
  int ret_convert = Configuration::ConvertCommandlineToConfig(cl, config);

  // check result
  EXPECT_EQ(1, ret_convert);
  EXPECT_EQ("", config.view.window_type);
}
