/*
 * Copyright 2026 Toyota Connected North America
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

#include "backend/drm_kms_egl/scene_layer_source_gl.h"

#include <utility>

#include <drm_fourcc.h>
#include <gbm.h>

#include "backend/drm_kms_egl/drm_compositor.h"

GbmBackingStoreLayerSource::GbmBackingStoreLayerSource(
    GbmBackingStore* store,
    EnsureFbIdFn ensure_fb_id)
    : store_(store), ensure_fb_id_(std::move(ensure_fb_id)) {}

drm::expected<drm::scene::AcquiredBuffer, std::error_code>
GbmBackingStoreLayerSource::acquire() {
  // The active slot is what Flutter most-recently rendered into. The
  // scene's commit window is short enough that slot rotation (which
  // happens in DrmCompositor::PresentLayers after commit) cannot move
  // it under us mid-acquire.
  if (!ensure_fb_id_(*store_) || store_->active().drm_fb_id == 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }
  return drm::scene::AcquiredBuffer{
      .fb_id = store_->active().drm_fb_id,
      .opaque = &store_->active(),
  };
}

void GbmBackingStoreLayerSource::release(
    drm::scene::AcquiredBuffer /*acquired*/) noexcept {
  // BS slot rotation + scanning-slot retirement are managed by
  // DrmCompositor's in_flight_slots_ / scanning_slots_ pipeline,
  // driven off PAGE_FLIP_EVENT on the platform task-runner thread.
  // The wrapper has no per-acquire ownership to release.
}

drm::scene::BindingModel GbmBackingStoreLayerSource::binding_model()
    const noexcept {
  return drm::scene::BindingModel::SceneSubmitsFbId;
}

drm::scene::SourceFormat GbmBackingStoreLayerSource::format() const noexcept {
  // Query the live BO's modifier rather than caching at construction —
  // a pool-recycled store may have a different modifier than the one
  // we last saw. width/height/format are stable across slots.
  std::uint64_t mod = DRM_FORMAT_MOD_INVALID;
  if (auto* bo = store_->active().bo) {
    mod = gbm_bo_get_modifier(bo);
  }
  return drm::scene::SourceFormat{
      .drm_fourcc = store_->format,
      .modifier = mod,
      .width = store_->width,
      .height = store_->height,
  };
}
