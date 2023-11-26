#include <filesystem>
#include <stdexcept>

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

#include <configuration/configuration.h>
#include <rapidjson/document.h>

static constexpr char kSourceRoot[] = SOURCE_ROOT_DIR;

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationParseConfig_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test ParseConfig for default value
***************************************************************/

TEST(HomescreenConfigurationParseConfig, Lv1Normal001) {
  struct Configuration::Config config {};
  config.view.bundle_path = "/home/root/";

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::ParseConfig(config);

  Configuration::Config config_ret = configs.back();

  EXPECT_EQ("homescreen", config_ret.app_id);
  EXPECT_EQ(false, config_ret.disable_cursor);
  EXPECT_EQ(false, config_ret.debug_backend);
  EXPECT_EQ(false, config_ret.debug_backend);
  EXPECT_EQ("/home/root/", config_ret.view.bundle_path);
  EXPECT_EQ("NORMAL", config_ret.view.window_type);
  EXPECT_EQ(0, config_ret.view.wl_output_index);
  EXPECT_EQ(1920, config_ret.view.width);
  EXPECT_EQ(720, config_ret.view.height);
  EXPECT_EQ(false, config_ret.view.fullscreen);
  EXPECT_EQ(1, config_ret.view.pixel_ratio);
  EXPECT_EQ(0, config_ret.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationParseConfig_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test ParseConfig using json file
***************************************************************/

TEST(HomescreenConfigurationParseConfig, Lv1Normal002) {
  struct Configuration::Config config {};
  std::string src_root = kSourceRoot;
  config.json_configuration_path = src_root + "/files/normal_1.json";

  // call target function
  std::vector<struct Configuration::Config> configs = Configuration::ParseConfig(config);

  Configuration::Config config_ret = configs.back();

  EXPECT_EQ("homescreen", config_ret.app_id);
  EXPECT_EQ(true, config_ret.disable_cursor);
  EXPECT_EQ(true, config_ret.debug_backend);
  EXPECT_EQ(true, config_ret.debug_backend);
  EXPECT_EQ("/usr/share/", config_ret.view.bundle_path);
  EXPECT_EQ("BG", config_ret.view.window_type);
  EXPECT_EQ(1, config_ret.view.wl_output_index);
  EXPECT_EQ(1280, config_ret.view.width);
  EXPECT_EQ(720, config_ret.view.height);
  EXPECT_EQ(true, config_ret.view.fullscreen);
  EXPECT_EQ(2.0, config_ret.view.pixel_ratio);
  EXPECT_EQ(0, config_ret.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name：
HomescreenConfigurationConfigFromArgcArgv_Lv1Normal001 Use Case Name:
Initialization Test Summary：Test the function of ConfigFromArgcArgv
***************************************************************/

TEST(HomescreenConfigurationConfigFromArgcArgv, Lv1Normal001) {
  // setup test parameters
  struct Configuration::Config config {};
  int argc = 8;
  const char* argv[8] = {"homescreen",
                         "--b=/home",
                         "--a=1",
                         "--w=1920",
                         "--h=720",
                         "--p=1",
                         "--window-type=NORMAL",
                         "--xdg-shell-app-id=homescreen"};
  char** argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  config = Configuration::ConfigFromArgcArgv(argc, argv_p);

  // check result
  EXPECT_EQ("homescreen", config.app_id);
  EXPECT_EQ(false, config.disable_cursor);
  EXPECT_EQ(false, config.debug_backend);

  EXPECT_EQ("/home", config.view.bundle_path);
  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(0, config.view.wl_output_index);
  EXPECT_EQ(1920, config.view.width);
  EXPECT_EQ(720, config.view.height);
  EXPECT_EQ(false, config.view.fullscreen);
  EXPECT_EQ(1, config.view.pixel_ratio);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationgetView_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test getView with view key
***************************************************************/

TEST(HomescreenConfigurationgetView, Lv1Normal001) {
  // set test parameters
  struct Configuration::Config config {};
  std::string json_str =
      "{"
      "\"app_id\":\"homescreen\","
      "\"cursor_theme\":\"DMZ-White\","
      "\"disable_cursor\":true,"
      "\"debug_backend\":true,"
      "\"bundle_path\":\"/home\","
      "\"window_type\":\"NORMAL\","
      "\"view\":["
        "{"
        "\"output_index\":1,"
        "\"width\":1280,"
        "\"height\":720,"
        "\"accessibility_features\":1,"
        "\"fullscreen\":true,"
        "\"ivi_surface_id\":1,"
        "\"pixel_ratio\":1"
        "}]"
      "}";
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());
  int index = 0;

  // call target function
  Configuration::getView(doc, index, config);

  // check result
  EXPECT_EQ("homescreen", config.app_id);
  EXPECT_EQ("DMZ-White", config.cursor_theme);
  EXPECT_EQ(true, config.disable_cursor);
  EXPECT_EQ(true, config.debug_backend);

  EXPECT_EQ("/home", config.view.bundle_path);
  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(1, config.view.wl_output_index);
  EXPECT_EQ(1280, config.view.width);
  EXPECT_EQ(720, config.view.height);
  EXPECT_EQ(true, config.view.fullscreen);
  EXPECT_EQ(1, config.view.ivi_surface_id);
  EXPECT_EQ(1, config.view.pixel_ratio);
  EXPECT_EQ(1, config.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationgetView_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test getView without view param
***************************************************************/

TEST(HomescreenConfigurationgetView, Lv1Normal002) {
  // set test parameters
  struct Configuration::Config config {};
  std::string json_str =
      "{"
      "\"app_id\":\"homescreen\","
      "\"cursor_theme\":\"DMZ-White\","
      "\"disable_cursor\":true,"
      "\"debug_backend\":true,"
      "\"bundle_path\":\"/home\","
      "\"window_type\":\"NORMAL\","
      "\"view\":{}"
      "}";
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());
  int index = 0;

  // call target function
  Configuration::getView(doc, index, config);

  // check result
  EXPECT_EQ("homescreen", config.app_id);
  EXPECT_EQ("DMZ-White", config.cursor_theme);
  EXPECT_EQ(true, config.disable_cursor);
  EXPECT_EQ(true, config.debug_backend);

  EXPECT_EQ("/home", config.view.bundle_path);
  EXPECT_EQ("NORMAL", config.view.window_type);
  EXPECT_EQ(0, config.view.wl_output_index);
  EXPECT_EQ(0, config.view.width);
  EXPECT_EQ(0, config.view.height);
  EXPECT_EQ(false, config.view.fullscreen);
  EXPECT_EQ(0, config.view.ivi_surface_id);
  EXPECT_EQ(0, config.view.pixel_ratio);
  EXPECT_EQ(0, config.view.accessibility_features);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetViewCount_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of getViewCount
***************************************************************/

TEST(HomescreenConfigurationgetViewCount, Lv1Normal001) {
  // set test parameters
  char const* json_str = R"testParams({
      "app_id " : "homescreen",
      "view" : [
          {
              "window_type" : "BG"
          },
          {
              "window_type" : "NORMAL"
          }
      ]
  })testParams";
  rapidjson::Document doc;
  doc.Parse(json_str);

  // call target function
  rapidjson::SizeType ret_count = Configuration::getViewCount(doc);

  // check result
  EXPECT_EQ(2, ret_count);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationgetViewCount_Lv1Abormal001
Use Case Name: Initialization
Test Summary：Test getViewCount with non-array view date
***************************************************************/

TEST(HomescreenConfigurationgetViewCount, Lv1Normal002) {
  // set test parameters
  std::string json_str = "{"
        "\"app_id\":\"homescreen\","
        "\"view\":\"abnormal\""
    "}";
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  // call target function
  rapidjson::SizeType ret_count = Configuration::getViewCount(doc);

  // check result
  EXPECT_EQ(1, ret_count);
}

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationGetJsonDocument_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of getJsonDocument
***************************************************************/

TEST(HomescreenConfigurationGetJsonDocument, Lv1Normal001) {
  // set test parameters
  struct Configuration::Config config {};

  // call target function
  std::string src_root = kSourceRoot;
  rapidjson::Document doc =
      Configuration::getJsonDocument(src_root + "/files/normal_1.json");

  // check result
  EXPECT_STREQ("homescreen", doc["app_id"].GetString());
  EXPECT_EQ(true, doc["disable_cursor"].GetBool());
  EXPECT_EQ(true, doc["debug_backend"].GetBool());

  EXPECT_STREQ("/usr/share/", doc["view"][0]["bundle_path"].GetString());
  EXPECT_STREQ("BG", doc["view"][0]["window_type"].GetString());
  EXPECT_EQ(1, doc["view"][0]["output_index"].GetInt());
  EXPECT_EQ(1280, doc["view"][0]["width"].GetInt());
  EXPECT_EQ(720, doc["view"][0]["height"].GetInt());
  EXPECT_EQ(2.0, doc["view"][0]["pixel_ratio"].GetDouble());
  EXPECT_EQ(true, doc["view"][0]["fullscreen"].GetBool());
  EXPECT_EQ(1, doc["view"][0]["ivi_surface_id"].GetInt());
}
