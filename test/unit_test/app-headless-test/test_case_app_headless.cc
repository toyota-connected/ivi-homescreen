#include "gtest/gtest.h"

#include <dlfcn.h>
#include <cassert>
#include <stdexcept>

#include <EGL/egl.h>

#include "app.h"
#include "backend/headless/headless.h"
#include "configuration/configuration.h"
#include "logging.h"
#include "unit_test_utils.h"



/****************************************************************
Test Case Name.Test Name： HomescreenAppHeadless_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop without window_type
***************************************************************/
TEST(HomescreenAppHeadless, Lv1Normal001) {
  constexpr int argc = 3;
  const char* argv[3] = {"homescreen", "-b", kBundlePath};
  const auto argv_p = reinterpret_cast<char**>(&argv);

  // call target function
  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  App app(configs);
  int ret = app.Loop();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(2s);

  // run the application until failure or queue empty
  do {
    ret = app.Loop();
  } while (ret > 0);

  const auto test_filename = utils_get_image_filename(TEST, "1");
  const auto golden_filename = utils_get_image_filename(GOLDEN, "1");

#if SAVE_IMAGE_FOR_COMPARISON
  utils_write_targa(app.getViewRenderBuf(0), golden_filename,
                    static_cast<int>(configs[0].view.width.value_or(1920)),
                    static_cast<int>(configs[0].view.height.value_or(1080)));
#endif
#if BUILD_BACKEND_HEADLESS_EGL
  utils_write_targa(app.getViewRenderBuf(0), test_filename,
                    static_cast<int>(configs[0].view.width.value_or(1920)),
                    static_cast<int>(configs[0].view.height.value_or(1080)));
  const int images_are_equal = utils_images_are_equal(
      test_filename, golden_filename,
      static_cast<int>(configs[0].view.width.value_or(1920)),
      static_cast<int>(configs[0].view.height.value_or(1080)));

  EXPECT_EQ(images_are_equal, 1);
#endif
}

