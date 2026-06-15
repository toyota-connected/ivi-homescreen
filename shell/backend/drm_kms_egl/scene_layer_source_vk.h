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

#include <cstdint>
#include <memory>
#include <system_error>

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>

namespace drm {
class Device;
}  // namespace drm

// drm::scene::LayerBufferSource adapter for a Flutter Vulkan backing
// store (kFlutterBackingStoreTypeVulkan) whose VkImage has already been
// exported to a DMA-BUF by the renderer-side integration.
//
// The Vulkan engine path ships dormant in ivi-homescreen today — there
// is no live BUILD_BACKEND_DRM_KMS_VULKAN backend yet — but the seam is
// in place so the eventual Impeller-on-Linux integration is a renderer-
// config flip plus a CreateBackingStore switch arm, not an architectural
// change.
//
// The VkImage→dmabuf export (vkGetMemoryFdKHR via VK_KHR_external_memory_fd,
// plus a VkExternalMemoryImageCreateInfo on image creation) lives in the
// caller — this wrapper takes the already-exported (fd, format, modifier,
// plane layout) tuple. That keeps the wrapper free of Vulkan headers and
// keeps USE_DRM_SCENE off the BUILD_BACKEND_VULKAN dependency graph.
//
// Sync is an OPEN QUESTION, not a solved one. FlutterVulkanImage carries
// only {image, format} — the embedder exposes no per-backing-store
// render-complete fence or semaphore, and was empirically shown (issue
// #208) to rely on shared-queue submission ordering rather than an
// observable hand-off. Whether the exported dma-buf is safe to scan out
// at present_layers time therefore depends on either the engine host-
// idling its raster queue before present_layers, or the exported dma-buf
// carrying an implicit dma_resv fence KMS auto-waits on — neither is
// confirmed. This must be resolved by the render-complete hand-off spike
// before relying on it. If explicit sync turns
// out to be required, it belongs on ExternalDmaBufSource's IN_FENCE_FD
// path, not here — this wrapper stays deliberately sync-agnostic.
//
// The wrapper caches one ExternalDmaBufSource per VkImage; on first sight
// the caller constructs the wrapper with the dmabuf fd (one per VkImage;
// the wrapper dups it internally via ExternalDmaBufSource), on subsequent
// frames the same wrapper is reused for the same VkImage.
class VkBackingStoreLayerSource final : public drm::scene::LayerBufferSource {
 public:
  // Construct over a caller-exported dmabuf. The wrapper builds an
  // ExternalDmaBufSource that owns the duped fd and the resulting KMS
  // framebuffer.
  //
  // The 1..4-plane / LINEAR-or-INVALID modifier restriction is inherited
  // from ExternalDmaBufSource — see its header for the validation
  // contract. Tiled modifiers (X_TILED, AFBC, etc.) are out of scope and
  // would require explicit driver-side negotiation per renderer.
  [[nodiscard]] static drm::expected<std::unique_ptr<VkBackingStoreLayerSource>,
                                     std::error_code>
  create(const drm::Device& dev,
         std::uint32_t width,
         std::uint32_t height,
         std::uint32_t drm_fourcc,
         std::uint64_t modifier,
         drm::span<const drm::scene::ExternalPlaneInfo> planes);

  ~VkBackingStoreLayerSource() override = default;

  VkBackingStoreLayerSource(const VkBackingStoreLayerSource&) = delete;
  VkBackingStoreLayerSource& operator=(const VkBackingStoreLayerSource&) =
      delete;

  // ── LayerBufferSource — forwarded to inner ExternalDmaBufSource. ────
  [[nodiscard]] drm::expected<drm::scene::AcquiredBuffer, std::error_code>
  acquire() override {
    return inner_->acquire();
  }
  void release(drm::scene::AcquiredBuffer acquired) noexcept override {
    inner_->release(std::move(acquired));
  }
  [[nodiscard]] drm::scene::BindingModel binding_model()
      const noexcept override {
    return inner_->binding_model();
  }
  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override {
    return inner_->format();
  }
  void on_session_paused() noexcept override { inner_->on_session_paused(); }
  drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override {
    return inner_->on_session_resumed(new_dev);
  }

 private:
  explicit VkBackingStoreLayerSource(
      std::unique_ptr<drm::scene::ExternalDmaBufSource> inner)
      : inner_(std::move(inner)) {}

  std::unique_ptr<drm::scene::ExternalDmaBufSource> inner_;
};
