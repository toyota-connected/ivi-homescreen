#pragma once

#include <string>

namespace view_filament_view::models::state {

/// Represents the state of the model.
enum class ModelState {
  /// represents idle state.
  none,

  /// represents loading state.
  loading,

  /// represents the model has loaded successfully.
  loaded,

  /// represents the model has failed loading and it loaded the fallback model
  /// successfully.
  fallbackLoaded,

  /// represents the model and fallback model have failed loading.
  error,
};

static constexpr char kModelStateNone[] = "NONE";
static constexpr char kModelStateLoading[] = "LOADING";
static constexpr char kModelStateLoaded[] = "LOADED";
static constexpr char kModelStateFallbackLoaded[] = "FALLBACK_LOADED";
static constexpr char kModelStateError[] = "ERROR";

static ModelState getModelStateForText(const std::string& state) {
  if (state == kModelStateNone) {
    return ModelState::none;
  }
  if (state == kModelStateLoading) {
    return ModelState::loading;
  }
  if (state == kModelStateLoaded) {
    return ModelState::loaded;
  }
  if (state == kModelStateFallbackLoaded) {
    return ModelState::fallbackLoaded;
  }
  if (state == kModelStateError) {
    return ModelState::error;
  }
  return ModelState::none;
}

static const char* getTextForModelState(ModelState state) {
  return (const char*[]){
      kModelStateNone,           kModelStateLoading, kModelStateLoaded,
      kModelStateFallbackLoaded, kModelStateError,
  }[static_cast<int>(state)];
}

}  // namespace view_filament_view::models::state
