#include "gtest/gtest.h"

#include "app.h"
#include "configuration/configuration.h"
#include "logging/logger.hpp"
#include "unit_test_utils.h"

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop without window_type
***************************************************************/

TEST(HomescreenAppLoop, Lv1Normal001) {
  SKIP_WITHOUT_APP_BUNDLE();
  constexpr int argc = 3;
  const char* argv[3] = {"homescreen", "-b", kBundlePath};
  const auto argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  const Configuration::Config& config = configs.back();
  Configuration::PrintConfig(config);

  const App app(configs);
  (void)app.Loop();

  // No checks/assertions, if method succeeds, program will continue.  If it
  // fails, program should abort, which will fail this test.
}

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop with window_type BG
***************************************************************/

TEST(HomescreenAppLoop, Lv1Normal002) {
  SKIP_WITHOUT_APP_BUNDLE();
  constexpr int argc = 5;
  const char* argv[5] = {"homescreen", "-b", kBundlePath, "--window-type",
                         "BG"};
  const auto argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  const Configuration::Config& config = configs.back();
  Configuration::PrintConfig(config);

  const App app(configs);
  (void)app.Loop();

  // No checks/assertions, if method succeeds, program will continue.  If it
  // fails, program should abort, which will fail this test.
}

// Own main, so the shell's logging is running for the duration of the tests.
// ihs::log::* gates every level -- critical included -- on a valid
// IhsLogContext, and a context is only valid once ihs_log_start() has run.
// gtest_main never calls it, so a test binary that does not do this itself
// discards every diagnostic the code under test emits, silently: #685's
// "exits 1 with no output" was a critical from Configuration::ParseArgcArgv
// going nowhere. Same shape as cache_dir-test and the two drm_backend_vkms
// tests. Linking gtest_main alongside is fine: the linker does not pull its
// main() when this object already defines one.
int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "app test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
