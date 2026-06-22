#include "gtest/gtest.h"

#include <dlfcn.h>
#include <cassert>
#include <stdexcept>

#include "app.h"
#include "backend/software/memory_sink.h"
#include "backend/software/software_backend.h"
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

#if BUILD_BACKEND_HEADLESS_SOFTWARE
  size_t row_bytes = 0;
  size_t frame_height = 0;
  const auto buf = app.getViewRenderBuf(0, &row_bytes, &frame_height);
  const auto w = static_cast<int>(configs[0].view.width.value_or(1920));
  const auto h = static_cast<int>(configs[0].view.height.value_or(1080));
#if SAVE_IMAGE_FOR_COMPARISON
  utils_write_targa(buf.data(), golden_filename, w, h);
#endif
  utils_write_targa(buf.data(), test_filename, w, h);
  const int images_are_equal =
      utils_images_are_equal(test_filename, golden_filename, w, h);
  EXPECT_EQ(images_are_equal, 1);
#endif
}
