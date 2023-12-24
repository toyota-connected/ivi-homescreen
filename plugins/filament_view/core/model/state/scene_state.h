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

/// Represents the state of the scene.
enum class SceneState {
  /// represents idle state.
  NONE,

  /// represents loading state.
  LOADING,

  /// represents the scene has finished loading successfully.
  LOADED,

  /// represents that some error happened while loading the scene.
  ERROR,
};

static constexpr char kSceneStateNone[] = "NONE";
static constexpr char kSceneStateLoading[] = "LOADING";
static constexpr char kSceneStateLoaded[] = "LOADED";
static constexpr char kSceneStateError[] = "ERROR";

[[maybe_unused]] static SceneState getSceneStateForText(
    const std::string& state) {
  if (state == kSceneStateNone) {
    return SceneState::NONE;
  } else if (state == kSceneStateLoading) {
    return SceneState::LOADING;
  } else if (state == kSceneStateLoaded) {
    return SceneState::LOADED;
  } else if (state == kSceneStateError) {
    return SceneState::ERROR;
  }
  return SceneState::NONE;
}

[[maybe_unused]] static const char* getTextForSceneState(SceneState state) {
  return (const char*[]){
      /// represents idle state.
      kSceneStateNone,

      /// represents loading state.
      kSceneStateLoading,

      /// represents the model has loaded successfully.
      kSceneStateLoaded,

      /// represents the model and fallback model have failed loading.
      kSceneStateError,
  }[static_cast<int>(state)];
}

}  // namespace plugin_filament_view
