
#include "hdr_loader.h"

#include <fstream>
#include <sstream>

#include <filament/Engine.h>
#include <filament/Texture.h>
#include <image/LinearImage.h>
#include <imageio/ImageDecoder.h>

#include "logging/logging.h"

namespace plugin_filament_view {

using namespace filament;
using namespace image;
using namespace utils;

::filament::Texture* HDRLoader::createTexture(::filament::Engine* engine,
                                              const std::string& asset_path) {
  SPDLOG_DEBUG("Loading {}", asset_path.c_str());

  // Load the PNG file at the given path.
  std::ifstream ins(asset_path, std::ios::binary);
  auto* image = new LinearImage(ImageDecoder::decode(ins, "memory.hdr"));

  // This can happen if a decoding error occurs.
  if (image->getChannels() != 3) {
    delete image;
    return nullptr;
  }

  Texture* texture = Texture::Builder()
                         .width(image->getWidth())
                         .height(image->getHeight())
                         .levels(0xff)
                         .format(Texture::InternalFormat::R11F_G11F_B10F)
                         .sampler(Texture::Sampler::SAMPLER_2D)
                         .build(*engine);

  if (texture == nullptr) {
    spdlog::error("Unable to create Filament Texture from HDR image.");
    delete image;
    return nullptr;
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

  // Note that the setImage call could fail (e.g., due to an invalid combination
  // of internal format and PixelDataFormat) but there is no way of detecting
  // such a failure.
  texture->setImage(*engine, 0, std::move(pbd));

  texture->generateMipmaps(*engine);

  return texture;
}

::filament::Texture* HDRLoader::createTexture(
    ::filament::Engine* engine,
    const std::vector<uint8_t>& buffer) {
  // Load the PNG file from the buffer.
  std::string str(buffer.begin(), buffer.end());
  std::istringstream ins(str);
  auto* image = new LinearImage(ImageDecoder::decode(ins, "memory.hdr"));

  // This can happen if a decoding error occurs.
  if (image->getChannels() != 3) {
    delete image;
    return nullptr;
  }

  Texture* texture = Texture::Builder()
                         .width(image->getWidth())
                         .height(image->getHeight())
                         .levels(0xff)
                         .format(Texture::InternalFormat::R11F_G11F_B10F)
                         .sampler(Texture::Sampler::SAMPLER_2D)
                         .build(*engine);

  if (texture == nullptr) {
    spdlog::error("Unable to create Filament Texture from HDR image.");
    delete image;
    return nullptr;
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

  // Note that the setImage call could fail (e.g., due to an invalid combination
  // of internal format and PixelDataFormat) but there is no way of detecting
  // such a failure.
  texture->setImage(*engine, 0, std::move(pbd));

  texture->generateMipmaps(*engine);

  return texture;
}
}  // namespace plugin_filament_view