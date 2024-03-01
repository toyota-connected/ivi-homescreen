#include "unit_test_utils.h"
#include "logging/logging.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::string utils_get_image_filename(uint8_t type, std::string idx) {
  const testing::TestInfo* const test_info =
    testing::UnitTest::GetInstance()->current_test_info();
  std::stringstream filename;

  if(type == TEST) {
    filename << kTestImagePath << test_info->test_suite_name() << "_" << test_info->name() << "_TEST_" << idx << ".tga";
  } else {
    filename << kGoldenImagePath << test_info->test_suite_name() << "_" << test_info->name() << "_GOLDEN_" << idx << ".tga";
  }

  return filename.str();
}

void utils_write_targa(uint8_t* buf, const char *filename, int width, int height)
{
  #pragma pack(push, 1)
  typedef struct {
    uint8_t id_length;
    uint8_t color_map_type;
    uint8_t image_type;
    struct {
      uint16_t index;
      uint16_t length;
      uint8_t size;
    } color_map;
    struct {
      uint16_t x;
      uint16_t y;
    } origin;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_depth;
    uint8_t image_descriptor;
  } TARGA_HEADER;

  typedef struct {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
  } BGR_PIXEL;
  #pragma pack(pop)


  TARGA_HEADER header{};
  header.image_type = 2;
  header.width = static_cast<uint16_t>(width);
  header.height = static_cast<uint16_t>(height);
#if (IS_ARCH_BIG_ENDIAN)
  spdlog::info("Swapping bytes due to endianness");
  header.width = ((header.width & 0xFF) << 8) | ((header.width & 0xFF00) >> 8);
  header.height = ((header.height & 0xFF) << 8) | ((header.height & 0xFF00) >> 8);
#endif
  header.pixel_depth = 0x18;
  header.image_descriptor = 0x20;

  spdlog::info("Writing buffer to Targa file.");
  auto f = fopen(filename, "wb");
  if(f) {
    fwrite(&header, sizeof(TARGA_HEADER), 1, f);

    int i, x, y;
    for (y = height - 1; y >= 0; y--) {
      for (x = 0; x < width; x++) {
        i = (y * width + x) * 4;
        BGR_PIXEL pixel{
            .blue  = buf[i + 2],
            .green = buf[i + 1],
            .red =   buf[i]
        };
        fwrite(&pixel, sizeof(BGR_PIXEL), 1, f);
      }
    }
    fclose(f);
  } else {
    spdlog::info("File/Path not found: {}", filename);
  }
}

#define IMAGE_FILE_SIZE(h, w) (((h) * (w) * 3u) + 18u)
int utils_images_are_equal(const char* image_under_test,
                           const char* image_comp,
                           int width,
                           int height) {
  int ret;
  bool failed = false;
  uint8_t* test_image_buf = (uint8_t*)malloc(IMAGE_FILE_SIZE(height, width));
  uint8_t* comp_image_buf = (uint8_t*)malloc(IMAGE_FILE_SIZE(height, width));
  FILE* test_image = fopen(image_under_test, "r");
  FILE* comp_image = fopen(image_comp, "r");

  spdlog::info("Comparing images:\n\t{}\n\t{}", image_under_test, image_comp);
  if ((test_image) && (comp_image)) {
      size_t r1 =
          fread(test_image_buf, 1, (size_t)IMAGE_FILE_SIZE(height, width), test_image);
      size_t r2 =
          fread(comp_image_buf, 1, (size_t)IMAGE_FILE_SIZE(height, width), comp_image);

      if (r1 != r2 || memcmp(test_image_buf, comp_image_buf, r1)) {
        ret = 0;
      }
      else {
        ret = 1;
      }
  } else {
    spdlog::info("Could not open file(s)");
    ret = -1;
  }
  if(test_image) {
    free(test_image_buf);
    fclose(test_image);
  }
  if(comp_image) {
    free(comp_image_buf);
    fclose(comp_image);
  }

  return ret;
}