#pragma once

#include <cstdint>
#include <cstring>
#include "gtest/gtest.h"

enum ImageType { TEST = 0, GOLDEN };

static constexpr char kBundlePath[] = TEST_APP_BUNDLE_PATH;
static std::string kBundlePathStr = TEST_APP_BUNDLE_PATH;
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