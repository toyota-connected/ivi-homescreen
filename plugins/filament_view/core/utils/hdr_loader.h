#pragma once

#include <filament/Engine.h>
#include <filament/Texture.h>
#include <image/LinearImage.h>

namespace plugin_filament_view {

class HDRLoader {
 public:
  static ::filament::Texture* createTexture(::filament::Engine* engine,
                                            const std::string& asset_path,
                                            const std::string& name = "memory.hdr");

  static ::filament::Texture* createTexture(::filament::Engine* engine,
                                            const std::vector<uint8_t>& buffer,
                                            const std::string& name = "memory.hdr");

 private:
  static ::filament::Texture* deleteImageAndLogError(image::LinearImage* image);

  static ::filament::Texture* createTextureFromImage(::filament::Engine* engine,
                                                     image::LinearImage* image);
};

}  // namespace plugin_filament_view
