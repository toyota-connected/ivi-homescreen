#include "gtest/gtest.h"
#include "logging/logger.hpp"
#include "unit_test_utils.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

FlutterView* createFlutterViewInstance() {
  int argc = 3;
  const char* argv[3] = {"homescreen", "-b", kBundlePath};
  char** argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  Configuration::Config config = configs.back();

  auto wayland_display = std::make_shared<Display>(false, "", "", configs);
  // The third argument is the view's name, which App derives from the bundle
  // directory (App::NameForView). That is private to App, and the name only
  // shows up in log lines, so a literal does here.
  auto* view = new FlutterView(config, 0, "flutter_view-test", wayland_display);
  return view;
}

/****************************************************************
Test Case Name.Test Name： HomescreenFlutterViewConstructor_Lv1Normal001
Use Case Name: Provide wayland client function
Test Summary：Test the constructor of FlutterView class
***************************************************************/
TEST(HomescreenFlutterViewConstructor, Lv1Normal001) {
  SKIP_WITHOUT_APP_BUNDLE();
  // call target function
  FlutterView* view = createFlutterViewInstance();

  // confirm to create instance
  EXPECT_TRUE(view != nullptr);
}

// See the note in app-test: ihs::log::* emits nothing until ihs_log_start()
// has run, and gtest_main does not call it. This test reaches the same
// Configuration::ParseArgcArgv rejection, so without this its diagnostics
// would vanish the same way.
int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "flutter_view test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
