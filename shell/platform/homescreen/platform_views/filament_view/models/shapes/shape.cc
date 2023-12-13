
#include "shape.h"

#include <flutter/encodable_value.h>

namespace view_filament_view {

Shape::Shape(void* parent,
             const std::string& flutter_assets_path,
             const flutter::EncodableMap& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    spdlog::debug("Shape: {}", key);

    if (key == "id" && std::holds_alternative<int>(it.second)) {
      type_ = std::get<int>(it.second);
    } else if (key == "type" && std::holds_alternative<int32_t>(it.second)) {
      type_ = std::get<int32_t>(it.second);
    } else if (key == "centerPosition" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      centerPosition_ = std::make_unique<Position>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "normal" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      normal_ = std::make_unique<Direction>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "material" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      material_ = std::make_unique<Material>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Shape] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
}

void Shape::Print(const char* tag) const {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Shape)", tag);
  if (centerPosition_.has_value()) {
    centerPosition_.value()->Print("\tcenterPosition");
  }
  if (normal_.has_value()) {
    normal_.value()->Print("\tnormal");
  }
  if (material_.has_value()) {
    material_.value()->Print("\tsize");
  }
  spdlog::debug("++++++++");
}

}  // namespace view_filament_view