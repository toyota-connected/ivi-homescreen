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

#include <memory>

#include "encodable_value.h"

#include "exposure.h"
#include "lens_projection.h"
#include "platform_views/filament_view/models/scene/geometry/position.h"
#include "projection.h"
#include "shell/engine.h"

namespace view_filament_view {

class Projection;

class Camera {
 public:
  Camera(void* parent,
         const std::string& flutter_assets_path,
         const flutter::EncodableMap& params);
  void Print(const char* tag);
  FML_DISALLOW_COPY_AND_ASSIGN(Camera);
  enum class Mode {
    orbit,
    map,
    freeFlight,
  };

  enum class Fov {
    vertical,
    horizontal,
  };

  static const char* getTextForMode(Mode mode);
  static Mode getModeForText(const std::string& mode);
  static const char* getTextForFov(Fov fov);
  static Fov getFovForText(const std::string& fov);

 private:
  static constexpr char kModeOrbit[] = "ORBIT";
  static constexpr char kModeMap[] = "MAP";
  static constexpr char kModeFreeFlight[] = "FREE_FLIGHT";
  static constexpr char kFovVertical[] = "VERTICAL";
  static constexpr char kFovHorizontal[] = "HORIZONTAL";

  void* parent_;
  const std::string& flutterAssetsPath_;

  /// An object that control camera Exposure.
  std::optional<std::unique_ptr<Exposure>> exposure_;

  /// An object that controls camera projection matrix.
  std::optional<std::unique_ptr<Projection>> projection_;

  /// An object that control camera and set it's projection matrix from the
  /// focal length.
  std::optional<std::unique_ptr<LensProjection>> lensProjection_;

  /// Sets an additional matrix that scales the projection matrix.
  /// This is useful to adjust the aspect ratio of the camera independent from
  /// its projection.
  /// Its sent as List of 2 double elements :
  ///     * xscaling  horizontal scaling to be applied after the projection
  ///     matrix.
  //      * yscaling  vertical scaling to be applied after the projection
  //      matrix.
  std::optional<std::vector<double>> scaling_;

  ///      Sets an additional matrix that shifts (translates) the projection
  ///      matrix.
  ///     The shift parameters are specified in NDC coordinates.
  /// Its sent as List of 2 double elements :
  ///      *  xshift    horizontal shift in NDC coordinates applied after the
  ///      projection
  ///      *  yshift    vertical shift in NDC coordinates applied after the
  ///      projection
  std::optional<std::vector<double>> shift_;

  /// Mode of the camera that operates on.
  Mode mode_ = Mode::orbit;

  /// The world-space position of interest, which defaults to (x:0,y:0,z:-4).
  std::optional<std::unique_ptr<Position>> targetPosition_;

  /// The orientation for the home position, which defaults to (x:0,y:1,z:0).
  std::optional<std::unique_ptr<Position>> upVector_;

  /// The scroll delta multiplier, which defaults to 0.01.
  std::optional<double> zoomSpeed_;

  // orbit
  /// The initial eye position in world space for ORBIT mode.
  /// This defaults to (x:0,y:0,z:1).
  std::optional<std::unique_ptr<Position>> orbitHomePosition_;

  /// Sets the multiplier with viewport delta for ORBIT mode.This defaults to
  /// 0.01 List of 2 double :[x,y]
  std::optional<std::vector<double>> orbitSpeed_;

  /// The FOV axis that's held constant when the viewport changes.
  /// This defaults to Vertical.
  Camera::Fov fovDirection_ = Camera::Fov::vertical;

  /// The full FOV (not the half-angle) in the degrees.
  /// This defaults to 33.
  std::optional<double> fovDegrees_;

  /// The distance to the far plane, which defaults to 5000.
  std::optional<double> farPlane_;

  /// The ground plane size used to compute the home position for MAP mode.
  /// This defaults to 512 x 512
  std::optional<std::vector<double>> mapExtent_;

  /// Constrains the zoom-in level. Defaults to 0.
  std::optional<double> mapMinDistance_;

  /// The initial eye position in world space for FREE_FLIGHT mode.
  /// Defaults to (x:0,y:0,z:0).
  std::optional<std::unique_ptr<Position>> flightStartPosition_;

  /// The initial orientation in pitch and yaw for FREE_FLIGHT mode.
  /// Defaults to [0,0].
  std::optional<std::vector<double>> flightStartOrientation_;

  /// The maximum camera translation speed in world units per second for
  /// FREE_FLIGHT mode. Defaults to 10.
  std::optional<double> flightMaxMoveSpeed_;

  /// The number of speed steps adjustable with scroll wheel for FREE_FLIGHT
  /// mode.
  ///  Defaults to 80.
  std::optional<int64_t> flightSpeedSteps_;

  /// Applies a deceleration to camera movement in FREE_FLIGHT mode. Defaults to
  /// 0 (no damping). Lower values give slower damping times. A good default
  /// is 15.0. Too high a value may lead to instability.
  std::optional<double> flightMoveDamping_;

  /// The ground plane equation used for ray casts. This is a plane equation as
  /// in Ax + By + Cz + D = 0. Defaults to (0, 0, 1, 0).
  std::optional<std::vector<double>> groundPlane_;
};
}  // namespace view_filament_view