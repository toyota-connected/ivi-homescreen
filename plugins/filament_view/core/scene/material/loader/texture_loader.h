
#pragma once

#include "viewer/custom_model_viewer.h"

#include <future>

#include <filament/Texture.h>
#include <image/LinearImage.h>

#include <asio/io_context_strand.hpp>

#include "core/scene/material/texture/texture.h"

namespace plugin_filament_view {

class CustomModelViewer;

class Texture;

class TextureLoader {
 public:
  TextureLoader(CustomModelViewer* modelViewer, const std::string& assetPath);
  ~TextureLoader() = default;

  ::filament::Texture* loadTexture(Texture* texture);

  // Disallow copy and assign.
  TextureLoader(const TextureLoader&) = delete;
  TextureLoader& operator=(const TextureLoader&) = delete;

 private:
  CustomModelViewer* modelViewer_;
  const std::string& assetPath_;
  ::filament::Engine* engine_;
  const asio::io_context::strand& strand_;

  ::filament::Texture* createTextureFromImage(
      Texture::TextureType type,
      std::unique_ptr<image::LinearImage> image);

  ::filament::Texture* loadTextureFromStream(std::istream* ins,
                                             Texture::TextureType type,
                                             const std::string& name);

  ::filament::Texture* loadTextureFromUrl(std::string url,
                                          Texture::TextureType type);
};
}  // namespace plugin_filament_view