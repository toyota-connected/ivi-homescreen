/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Compositor-mode golden frame test running against the OSMesa headless
// backend. Same harness as test_case_app_headless.cc but compiled with
// BUILD_COMPOSITOR on. Establishes a parity gate: the single-layer fast
// path through the compositor must produce the same pixels as the
// non-compositor path. Once HeadlessBackend grows full compositor wiring
// (currently dormant — see docs/COMPOSITOR_GOLDENS.md) the test will
// exercise the create/collect/present callbacks instead of falling
// through to the engine's direct render path.
//
// Multi-layer goldens (PlatformViewLayer interleaving) require a Flutter
// bundle that emits PlatformViewSurface widgets — see the doc for the
// staging recipe.

#include <chrono>
#include <stdexcept>
#include <thread>

#include "app.h"
#include "configuration/configuration.h"
#include "gtest/gtest.h"
#include "logging.h"
#include "unit_test_utils.h"

TEST(CompositorHeadlessGolden, SingleLayerFastPathParity) {
  constexpr int argc = 3;
  const char* argv[3] = {"homescreen", "-b", kBundlePath};
  const auto argv_p = reinterpret_cast<char**>(&argv);

  const auto configs = Configuration::ParseArgcArgv(argc, argv_p);

  App app(configs);
  int ret = app.Loop();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(2s);

  do {
    ret = app.Loop();
  } while (ret > 0);

  const auto test_filename =
      utils_get_image_filename(TEST, "compositor_single_layer");
  const auto golden_filename =
      utils_get_image_filename(GOLDEN, "compositor_single_layer");

  const int width = static_cast<int>(configs[0].view.width.value_or(1920));
  const int height = static_cast<int>(configs[0].view.height.value_or(1080));

#if SAVE_IMAGE_FOR_COMPARISON
  // First-time generation. Inspect the image manually before committing.
  utils_write_targa(app.getViewRenderBuf(0), golden_filename, width, height);
  GTEST_FAIL() << "UNIT_TEST_SAVE_GOLDENS is on; this run only generates the "
                  "golden. Re-run with the option off to compare.";
#else
  utils_write_targa(app.getViewRenderBuf(0), test_filename, width, height);
  const int images_are_equal =
      utils_images_are_equal(test_filename, golden_filename, width, height);
  EXPECT_EQ(images_are_equal, 1)
      << "Compositor single-layer output diverged from baseline. "
         "If the divergence is intentional, regenerate the golden with "
         "-DUNIT_TEST_SAVE_GOLDENS=ON.";
#endif
}
