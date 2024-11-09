#include "unit_test_utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "logging/logging.h"

#define IMAGE_FILE_SIZE(h, w) static_cast<size_t>((((h) * (w) * 3) + 18))

namespace {
struct TARGA_HEADER {
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
};

struct BGR_PIXEL {
  uint8_t blue;
  uint8_t green;
  uint8_t red;
};

std::string buildFilename(const std::string& path,
                          const std::string& suite,
                          const std::string& name,
                          const std::string& type,
                          const std::string& idx) {
  std::stringstream filename;
  filename << path << suite << "_" << name << "_" << type << "_" << idx
           << ".tga";
  return filename.str();
}
}  // namespace

std::string utils_get_image_filename(const ImageType type,
                                     const std::string& idx) {
  const testing::TestInfo* const test_info =
      testing::UnitTest::GetInstance()->current_test_info();
  const std::string base_path =
      (type == TEST) ? kTestImagePath : kGoldenImagePath;
  const std::string type_str = (type == TEST) ? "TEST" : "GOLDEN";
  return buildFilename(base_path, test_info->test_suite_name(),
                       test_info->name(), type_str, idx);
}

void ensureDirectoryExistsIgnoringFileName(const std::filesystem::path& path) {
  if (const std::filesystem::path directoryPath =
          path.has_parent_path() ? path.parent_path() : path;
      !exists(directoryPath)) {
    if (create_directories(directoryPath)) {
      std::cout << "Directory created: " << directoryPath << '\n';
    } else {
      std::cerr << "Failed to create directory: " << directoryPath << '\n';
    }
  } else {
    std::cout << "Directory already exists: " << directoryPath << '\n';
  }
}

void utils_write_targa(const uint8_t* buf,
                       const std::string& filename,
                       int width,
                       int height) {
  TARGA_HEADER header{};
  header.image_type = 2;
  header.width = static_cast<uint16_t>(width);
  header.height = static_cast<uint16_t>(height);
#if (IS_ARCH_BIG_ENDIAN)
  spdlog::info("Swapping bytes due to endianness");
  header.width = ((header.width & 0xFF) << 8) | ((header.width & 0xFF00) >> 8);
  header.height =
      ((header.height & 0xFF) << 8) | ((header.height & 0xFF00) >> 8);
#endif
  header.pixel_depth = 0x18;
  header.image_descriptor = 0x20;

  ensureDirectoryExistsIgnoringFileName(filename);
  spdlog::info("Writing buffer to Targa file: {}", filename);
  if (const auto f = fopen(filename.c_str(), "wb")) {
    fwrite(&header, sizeof(TARGA_HEADER), 1, f);
    for (int y = height - 1; y >= 0; y--) {
      for (int x = 0; x < width; x++) {
        const int i = (y * width + x) * 4;
        BGR_PIXEL pixel{.blue = buf[i + 2], .green = buf[i + 1], .red = buf[i]};
        fwrite(&pixel, sizeof(BGR_PIXEL), 1, f);
      }
    }
    fclose(f);
  } else {
    spdlog::info("File/Path not found: {}", filename);
  }
}

int utils_images_are_equal(const std::string& image_under_test,
                           const std::string& image_comp,
                           const int width,
                           const int height) {
  int ret;

  auto* test_image_buf =
      static_cast<uint8_t*>(malloc(IMAGE_FILE_SIZE(height, width)));
  auto* comp_image_buf =
      static_cast<uint8_t*>(malloc(IMAGE_FILE_SIZE(height, width)));

  FILE* test_image = fopen(image_under_test.c_str(), "r");
  FILE* comp_image = fopen(image_comp.c_str(), "r");
  spdlog::info("Comparing images:\n\t{}\n\t{}", image_under_test, image_comp);

  if (test_image && comp_image) {
    const size_t r1 =
        fread(test_image_buf, 1, IMAGE_FILE_SIZE(height, width), test_image);
    const size_t r2 =
        fread(comp_image_buf, 1, IMAGE_FILE_SIZE(height, width), comp_image);
    ret = (r1 == r2 && memcmp(test_image_buf, comp_image_buf, r1) == 0) ? 1 : 0;
  } else {
    spdlog::info("Could not open file(s)");
    ret = -1;
  }

  if (test_image) {
    free(test_image_buf);
    fclose(test_image);
  }
  if (comp_image) {
    free(comp_image_buf);
    fclose(comp_image);
  }

  return ret;
}