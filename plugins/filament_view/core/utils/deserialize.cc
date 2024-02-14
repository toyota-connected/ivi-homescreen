
#include "deserialize.h"

namespace plugin_filament_view {

::filament::math::float3 Deserialize::Format3(
    const flutter::EncodableMap& map) {
  double x, y, z;
  x = y = z = 0.0f;

  for (auto& it : map) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "x" && std::holds_alternative<double>(it.second)) {
      x = std::get<double>(it.second);
    } else if (key == "y" && std::holds_alternative<double>(it.second)) {
      y = std::get<double>(it.second);
    } else if (key == "z" && std::holds_alternative<double>(it.second)) {
      z = std::get<double>(it.second);
    }
  }
  return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
}
}  // namespace plugin_filament_view
