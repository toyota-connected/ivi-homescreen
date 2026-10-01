#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include "gtest/gtest.h"

enum ImageType { TEST = 0, GOLDEN };

static constexpr char kBundlePath[] = TEST_APP_BUNDLE_PATH;
static std::string kBundlePathStr = TEST_APP_BUNDLE_PATH;

// Skip, rather than die, when there is no app bundle to run against.
//
// Configuration::ParseArgcArgv rejects a bundle path that is not a directory
// (configuration.cc:1240) and calls exit(EXIT_FAILURE). That takes the whole
// gtest process down mid-case: no failure is reported, and the ihs::log
// critical does not reach stderr from a test binary, so all the caller sees is
// "exit 1".
//
// TEST_APP_BUNDLE_PATH comes from UNIT_TEST_APP_BUNDLE, which CMakeLists.txt
// defaults to /home/root -- a path that exists on a target image and on neither
// a developer host nor a CI runner. Checking here states the requirement in the
// test output instead of hiding it in an exit code.
#define SKIP_WITHOUT_APP_BUNDLE()                                           \
  do {                                                                      \
    if (!std::filesystem::is_directory(kBundlePath)) {                      \
      GTEST_SKIP() << "no app bundle at " << kBundlePath                    \
                   << "; configure -DUNIT_TEST_APP_BUNDLE=<bundle dir> to " \
                      "run this case";                                      \
    }                                                                       \
  } while (false)
static constexpr char kGoldenImagePath[] = GOLDEN_IMAGE_PATH;
static constexpr char kTestImagePath[] = TEST_IMAGE_PATH;

std::string utils_get_image_filename(ImageType type, const std::string& idx);
void utils_write_targa(const uint8_t* buf,
                       const std::string& filename,
                       int width,
                       int height);
int utils_images_are_equal(const std::string& image_under_test,
                           const std::string& image_comp,
                           int width,
                           int height);