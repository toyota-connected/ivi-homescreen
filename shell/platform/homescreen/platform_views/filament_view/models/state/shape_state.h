#pragma once

#include <string>

namespace view_filament_view::models::state {

/// Represents the state of the Shape.
enum class ShapeState {
  /// represents idle state.
  none,

  /// represents loading state.
  loading,

  /// represents the scene has finished loading successfully.
  loaded,

  /// represents that some error happened while loading the scene.
  error,
};

static constexpr char kShapeStateNone[] = "NONE";
static constexpr char kShapeStateLoading[] = "LOADING";
static constexpr char kShapeStateLoaded[] = "LOADED";
static constexpr char kShapeStateError[] = "ERROR";

static ShapeState getSceneShapeForText(const std::string& state) {
  if (state == kShapeStateNone) {
    return ShapeState::none;
  } else if (state == kShapeStateLoading) {
    return ShapeState::loading;
  } else if (state == kShapeStateLoaded) {
    return ShapeState::loaded;
  } else if (state == kShapeStateError) {
    return ShapeState::error;
  }
  return ShapeState::none;
}

static const char* getTextForShapeState(ShapeState state) {
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

}  // namespace view_filament_view::models::state
