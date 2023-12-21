#pragma once

#include <filament/Material.h>
#include <filament/MaterialInstance.h>

#include "core/scene/material/loader/texture_loader.h"

namespace plugin_filament_view {

class TextureLoader;

class MaterialInstance : public ::filament::MaterialInstance {
 public:
  void setParameter(::filament::Material::ParameterInfo info, TextureLoader* loader);
};
}  // namespace plugin_filament_view