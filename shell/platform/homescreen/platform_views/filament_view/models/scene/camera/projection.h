/*
 * Copyright 2020-2023 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <optional>

#include "encodable_value.h"

#include "shell/engine.h"

namespace view_filament_view {

class Camera;

class Projection {
 public:
  Projection(void const* parent,
             const std::string& flutter_assets_path,
             const flutter::EncodableMap& params);
  void Print(const char* tag);
  FML_DISALLOW_COPY_AND_ASSIGN(Projection);
  enum class Type {
    /// Perspective projection, objects get smaller as they are farther.
    perspective,

    /// Orthonormal projection, preserves distances.
    ortho
  };

  enum class Fov {
    vertical,
    horizontal,
  };

  static const char* getTextForType(Type type);
  static Type getTypeForText(const std::string& type);
  static const char* getTextForFov(Fov fov);
  static Fov getFovForText(const std::string& fov);

 private:
  static constexpr char kTypePerspective[] = "PERSPECTIVE";
  static constexpr char kTypeOrtho[] = "ORTHO";
  static constexpr char kFovVertical[] = "VERTICAL";
  static constexpr char kFovHorizontal[] = "HORIZONTAL";

  const void* parent_;
  const std::string& flutterAssetsPath_;

  /// Denotes the projection type used by this camera.
  std::optional<Projection::Type> projection_;

  /// distance in world units from the camera to the left plane, at the near
  /// plane. Precondition: left != right
  std::optional<double> left_;

  /// distance in world units from the camera to the right plane, at the near
  /// plane. Precondition: left != right
  std::optional<double> right_;

  /// distance in world units from the camera to the bottom plane, at the near
  /// plane. Precondition: bottom != top
  std::optional<double> bottom_;

  /// distance in world units from the camera to the top plane, at the near
  /// plane. Precondition: bottom != top
  std::optional<double> top_;

  /// distance in world units from the camera to the near plane.
  ///  The near plane's position in view space is z = -near.
  ///  Precondition: near > 0 for ProjectionType.PERSPECTIVE or near != far for
  ///  ProjectionType.ORTHO.
  std::optional<double> near_;

  /// distance in world units from the camera to the far plane.
  ///  The far plane's position in view space is z = -far.
  ///  Precondition: far > near for ProjectionType.PERSPECTIVE or far != near
  ///  for ProjectionType.ORTHO.
  std::optional<double> far_;

  /// full field-of-view in degrees. 0 < fovInDegrees < 180
  std::optional<double> fovInDegrees_;

  /// aspect ratio width/height. aspect > 0
  std::optional<double> aspect_;

  /// direction of the field-of-view parameter.
  std::optional<Projection::Fov> fovDirection_;
};
}  // namespace view_filament_view