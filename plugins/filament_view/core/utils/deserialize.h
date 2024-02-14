
#pragma once

#include <filament/math/vec3.h>
#include <flutter/encodable_value.h>

namespace plugin_filament_view {

class Deserialize {
 public:
  Deserialize() = default;
  static ::filament::math::float3 Format3(const flutter::EncodableMap& map);
};
}
