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

#pragma once

#include <functional>
#include <system_error>

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

struct GbmBackingStore;

namespace drm {
class Device;
}  // namespace drm

// drm::scene::LayerBufferSource adapter over the GbmBackingStore that the
// DrmCompositor hands to Flutter as a kFlutterBackingStoreTypeOpenGL FBO.
//
// The store already owns the GBM BO, EGLImage, GL color texture, and a
// cached drm_fb_id that's lazily imported by DrmCompositor::EnsureDrmFbId.
// LayerScene only needs the KMS FB ID + the buffer's format/dimensions to
// drive direct scanout, so this adapter returns those without re-importing
// or re-wrapping anything — acquire() returns the cached fb_id (importing
// on first use via the EnsureFbIdFn callback), release() is a no-op (slot
// rotation is owned by DrmCompositor across Flutter's Create/Collect
// cycles), and the wrapper itself only borrows the store.
//
// BindingModel is SceneSubmitsFbId — the scene writes the cached fb_id to
// the plane's FB_ID property directly, no driver-side binding.
class GbmBackingStoreLayerSource final : public drm::scene::LayerBufferSource {
 public:
  // Callback that lazily imports the store's BO as a KMS framebuffer if
  // not already cached. DrmCompositor::EnsureDrmFbId is the natural impl
  // (the GL/EGL context it needs lives on the compositor); separating it
  // out keeps this header free of the EGL/GL machinery and lets the
  // wrapper be unit-testable against a fake.
  using EnsureFbIdFn = std::function<bool(GbmBackingStore&)>;

  // The store is borrowed — its lifetime is bounded by Flutter's
  // Create/Collect cycle and outlives this wrapper, which is held in
  // DrmCompositor::StoreBaton (released by CollectBackingStore).
  GbmBackingStoreLayerSource(GbmBackingStore* store, EnsureFbIdFn ensure_fb_id);
  ~GbmBackingStoreLayerSource() override = default;

  GbmBackingStoreLayerSource(const GbmBackingStoreLayerSource&) = delete;
  GbmBackingStoreLayerSource& operator=(const GbmBackingStoreLayerSource&) =
      delete;

  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;

  [[nodiscard]] drm::scene::BindingModel binding_model()
      const noexcept override;
  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override;

  // Session pause: drop nothing — DrmBackend's resume rebuilds the fd, and
  // EnsureDrmFbId re-imports on demand. The cached drm_fb_id will be
  // invalidated by DrmCompositor::OnResume's slot teardown so the next
  // acquire() re-imports against the new fd.
  void on_session_paused() noexcept override {}
  drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& /*new_dev*/) override {
    return {};
  }

  // Borrowed pointer accessor for compositor-side bookkeeping (identity
  // tag lookups, slot pipeline accounting).
  [[nodiscard]] GbmBackingStore* store() const noexcept { return store_; }

 private:
  GbmBackingStore* store_;
  EnsureFbIdFn ensure_fb_id_;
};
