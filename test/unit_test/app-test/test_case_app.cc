#include "gtest/gtest.h"

#include "app.h"
#include "configuration/configuration.h"

static constexpr char kBundlePath[] = TEST_APP_BUNDLE_PATH;

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop without window_type
***************************************************************/

TEST(HomescreenAppLoop, Lv1Normal001) {
  int argc = 3;
  const char* argv[3] = {"homescreen",
                         "-b",kBundlePath};
  char** argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  Configuration::Config config = configs.back();
  Configuration::PrintConfig(config);

  const App app(configs);
  int ret = app.Loop();

  // No checks/assertions, if method succeeds, program will continue.  If it
  // fails, program should abort, which will fail this test.
}

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop with window_type BG
***************************************************************/

TEST(HomescreenAppLoop, Lv1Normal002) {
  int argc = 5;
  const char* argv[5] = {"homescreen",
                         "-b",kBundlePath,
                         "--window-type","BG"};
  char** argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  Configuration::Config config = configs.back();
  Configuration::PrintConfig(config);

  App app(configs);
  int ret = app.Loop();

  // No checks/assertions, if method succeeds, program will continue.  If it
  // fails, program should abort, which will fail this test.
}
