#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>

enum {
    TEST = 0,
    GOLDEN
};

static constexpr char kBundlePath[] = TEST_APP_BUNDLE_PATH;
static constexpr char kGoldenImagePath[] = GOLDEN_IMAGE_PATH;
static constexpr char kTestImagePath[] = TEST_IMAGE_PATH;

std::string utils_get_image_filename(uint8_t type, std::string idx);
void utils_write_targa(uint8_t* buf, const char *filename, int width, int height);
int utils_images_are_equal(const char* image_under_test,
                           const char* image_comp,
                           int width,
                           int height);