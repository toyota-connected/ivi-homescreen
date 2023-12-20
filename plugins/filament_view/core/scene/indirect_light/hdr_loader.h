#pragma once

#include <filament/Engine.h>
#include <filament/Texture.h>

namespace plugin_filament_view {

class HDRLoader {
 public:
  static ::filament::Texture* createTexture(::filament::Engine* engine,
                                            const std::string& asset_path);
};

}  // namespace plugin_filament_view
