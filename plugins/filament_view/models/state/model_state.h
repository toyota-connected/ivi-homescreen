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

/// Represents the state of the model.
enum class ModelState {
  /// represents idle state.
  NONE,

  /// represents loading state.
  LOADING,

  /// represents the model has loaded successfully.
  LOADED,

  /// represents the model has failed loading and it loaded the fallback model
  /// successfully.
  FALLBACK_LOADED,

  /// represents the model and fallback model have failed loading.
  ERROR,
};

static constexpr char kModelStateNone[] = "NONE";
static constexpr char kModelStateLoading[] = "LOADING";
static constexpr char kModelStateLoaded[] = "LOADED";
static constexpr char kModelStateFallbackLoaded[] = "FALLBACK_LOADED";
static constexpr char kModelStateError[] = "ERROR";

[[maybe_unused]] static ModelState getModelStateForText(
    const std::string& state) {
  if (state == kModelStateNone) {
    return ModelState::NONE;
  }
  if (state == kModelStateLoading) {
    return ModelState::LOADING;
  }
  if (state == kModelStateLoaded) {
    return ModelState::LOADED;
  }
  if (state == kModelStateFallbackLoaded) {
    return ModelState::FALLBACK_LOADED;
  }
  if (state == kModelStateError) {
    return ModelState::ERROR;
  }
  return ModelState::NONE;
}

[[maybe_unused]] static const char* getTextForModelState(ModelState state) {
  return (const char*[]){
      kModelStateNone,           kModelStateLoading, kModelStateLoaded,
      kModelStateFallbackLoaded, kModelStateError,
  }[static_cast<int>(state)];
}

}  // namespace plugin_filament_view
