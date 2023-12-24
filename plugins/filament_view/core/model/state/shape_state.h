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

#include <string>

namespace plugin_filament_view {

/// Represents the state of the Shape.
enum class ShapeState {
  /// represents idle state.
  NONE,

  /// represents loading state.
  LOADING,

  /// represents the scene has finished loading successfully.
  LOADED,

  /// represents that some error happened while loading the scene.
  ERROR,
};

static constexpr char kShapeStateNone[] = "NONE";
static constexpr char kShapeStateLoading[] = "LOADING";
static constexpr char kShapeStateLoaded[] = "LOADED";
static constexpr char kShapeStateError[] = "ERROR";

[[maybe_unused]] static ShapeState getSceneShapeForText(
    const std::string& state) {
  if (state == kShapeStateNone) {
    return ShapeState::NONE;
  } else if (state == kShapeStateLoading) {
    return ShapeState::LOADING;
  } else if (state == kShapeStateLoaded) {
    return ShapeState::LOADED;
  } else if (state == kShapeStateError) {
    return ShapeState::ERROR;
  }
  return ShapeState::NONE;
}

[[maybe_unused]] static const char* getTextForShapeState(ShapeState state) {
  return (const char*[]){
      /// represents idle state.
      kShapeStateNone,

      /// represents loading state.
      kShapeStateLoading,

      /// represents the model has loaded successfully.
      kShapeStateLoaded,

      /// represents the model and fallback model have failed loading.
      kShapeStateError,
  }[static_cast<int>(state)];
}

}  // namespace plugin_filament_view
