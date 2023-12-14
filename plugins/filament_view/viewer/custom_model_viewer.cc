
#include "custom_model_viewer.h"

#include "models/state/model_state.h"
#include "logging/logging.h"

namespace plugin_filament_view {

CustomModelViewer::CustomModelViewer(void* parent)
    : parent_(parent), modelState_(models::state::ModelState::none) {
  cameraManager_ = std::make_unique<CameraManager>(this);
}

void CustomModelViewer::setModelState(models::state::ModelState modelState) {
  modelState_ = modelState;
  SPDLOG_DEBUG("[FilamentView] setModelState: {}",
               getTextForModelState(modelState_));
}
}  // namespace plugin_filament_view
