#include "gtest/gtest.h"

#include <dlfcn.h>
#include <cassert>
#include <sstream>
#include <stdexcept>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "app.h"
#include "backend/headless.h"
#include "configuration/configuration.h"
#include "unit_test_utils.h"
#include "logging.h"



/****************************************************************
Test Case Name.Test Name： HomescreenAppHeadless_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop without window_type
***************************************************************/
TEST(HomescreenAppHeadless, Lv1Normal001) {
  struct Configuration::Config config {};
  config.view.bundle_path = kBundlePath;
  config.json_configuration_path = config.view.bundle_path + "/default_config.json";

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::ParseConfig(config);

  App app(configs);
  int ret = app.Loop();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(2s);

  // run the application until failure or queue empty
  do {
    ret = app.Loop();
  } while (ret > 0);

  auto test_filename = utils_get_image_filename(TEST, "1");
  auto golden_filename = utils_get_image_filename(GOLDEN, "1");

#ifdef SAVE_IMAGE_FOR_COMPARISON
  utils_write_targa(app.getViewRenderBuf(0), golden_filename.c_str(), configs[0].view.width, configs[0].view.height);

  EXPECT_TRUE(0) << "Intentionally failed: Image saved for future comps"
                 << std::endl;
#else
  utils_write_targa(app.getViewRenderBuf(0), test_filename.c_str(), configs[0].view.width, configs[0].view.height);
  int images_are_equal =
    utils_images_are_equal(test_filename.c_str(), golden_filename.c_str(), configs[0].view.width, configs[0].view.height);

  EXPECT_EQ(images_are_equal, 1);
#endif
}

