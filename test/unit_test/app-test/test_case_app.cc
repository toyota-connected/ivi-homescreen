#include "gtest/gtest.h"

#include "app.h"
#include "configuration/configuration.h"

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop without window_type
***************************************************************/

TEST(HomescreenAppLoop, Lv1Normal001) {
  struct Configuration::Config config {};
  config.view.bundle_path = "/home/tcna/dev/workspace-automation/app/gallery/.desktop-homescreen";

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::ParseConfig(config);

  Configuration::PrintConfig(config);

  const App app(configs);
  int ret = app.Loop();

  // ret value is set at mock Display::PollEvents()
  EXPECT_EQ(1, ret);
}

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop with window_type BG
***************************************************************/

TEST(HomescreenAppLoop, Lv1Normal002) {
  struct Configuration::Config config {};
  config.view.bundle_path = "/home/root/";
  config.view.window_type = "BG";

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::ParseConfig(config);

  App app(configs);
  int ret = app.Loop();

  // ret value is set at mock Display::PollEvents()
  EXPECT_EQ(1, ret);
}
