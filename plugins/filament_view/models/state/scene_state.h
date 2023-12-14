#pragma once

#include <string>

namespace plugin_filament_view::models::state {

/// Represents the state of the scene.
enum class SceneState {
  /// represents idle state.
  none,

  /// represents loading state.
  loading,

  /// represents the scene has finished loading successfully.
  loaded,

  /// represents that some error happened while loading the scene.
  error,
};

static constexpr char kSceneStateNone[] = "NONE";
static constexpr char kSceneStateLoading[] = "LOADING";
static constexpr char kSceneStateLoaded[] = "LOADED";
static constexpr char kSceneStateError[] = "ERROR";

static SceneState getSceneStateForText(const std::string& state) {
  if (state == kSceneStateNone) {
    return SceneState::none;
  } else if (state == kSceneStateLoading) {
    return SceneState::loading;
  } else if (state == kSceneStateLoaded) {
    return SceneState::loaded;
  } else if (state == kSceneStateError) {
    return SceneState::error;
  }
  return SceneState::none;
}

static const char* getTextForSceneState(SceneState state) {
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

}  // namespace plugin_filament_view::models::state
