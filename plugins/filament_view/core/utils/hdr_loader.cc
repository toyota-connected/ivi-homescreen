#include "hdr_loader.h"

#include <fstream>
#include <sstream>

#include <imageio/ImageDecoder.h>

#include "logging/logging.h"

namespace plugin_filament_view {

using namespace filament;
using namespace image;
using namespace utils;

::filament::Texture* HDRLoader::deleteImageAndLogError(
    image::LinearImage* image) {
  spdlog::error("Unable to create Filament Texture from HDR image.");
  delete image;
  return nullptr;
}

::filament::Texture* HDRLoader::createTextureFromImage(
    ::filament::Engine* engine,
    image::LinearImage* image) {
  if (image->getChannels() != 3) {
    return deleteImageAndLogError(image);
  }

  Texture* texture = Texture::Builder()
                         .width(image->getWidth())
                         .height(image->getHeight())
                         .levels(0xff)
                         .format(Texture::InternalFormat::R11F_G11F_B10F)
                         .sampler(Texture::Sampler::SAMPLER_2D)
                         .build(*engine);

  if (!texture) {
    return deleteImageAndLogError(image);
  }

  Texture::PixelBufferDescriptor::Callback freeCallback =
      [](void* /* buf */, size_t, void* userdata) {
        delete (LinearImage*)userdata;
      };

  Texture::PixelBufferDescriptor pbd(
      (void const*)image->getPixelRef(),
      image->getWidth() * image->getHeight() * 3 * sizeof(float),
      Texture::PixelBufferDescriptor::PixelDataFormat::RGB,
      Texture::PixelBufferDescriptor::PixelDataType::FLOAT, freeCallback,
      image);

  texture->setImage(*engine, 0, std::move(pbd));
  texture->generateMipmaps(*engine);
  return texture;
}

::filament::Texture* HDRLoader::createTexture(::filament::Engine* engine,
                                              const std::string& asset_path,
                                              const std::string& name) {
  SPDLOG_DEBUG("Loading {}", asset_path.c_str());
  std::ifstream ins(asset_path, std::ios::binary);
  auto* image = new LinearImage(image::ImageDecoder::decode(ins, name));
  return createTextureFromImage(engine, image);
}

::filament::Texture* HDRLoader::createTexture(
    ::filament::Engine* engine,
    const std::vector<uint8_t>& buffer,
    const std::string& name) {
  std::string str(buffer.begin(), buffer.end());
  std::istringstream ins(str);
  auto* image = new LinearImage(image::ImageDecoder::decode(ins, name));
  return createTextureFromImage(engine, image);
}
}  // namespace plugin_filament_view