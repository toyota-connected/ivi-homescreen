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

#include "backend/drm_kms_egl/scene_layer_source_vk.h"

#include <utility>

drm::expected<std::unique_ptr<VkBackingStoreLayerSource>, std::error_code>
VkBackingStoreLayerSource::create(
    const drm::Device& dev,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t drm_fourcc,
    std::uint64_t modifier,
    drm::span<const drm::scene::ExternalPlaneInfo> planes) {
  auto inner = drm::scene::ExternalDmaBufSource::create(
      dev, width, height, drm_fourcc, modifier, planes);
  if (!inner) {
    return drm::unexpected(inner.error());
  }
  return std::unique_ptr<VkBackingStoreLayerSource>(
      new VkBackingStoreLayerSource(std::move(*inner)));
}
