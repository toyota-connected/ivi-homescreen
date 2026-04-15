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

#include <shell/platform/embedder/embedder.h>

/**
 * @brief Plugin-facing interface for a platform-view backing store producer.
 *
 * The compositor owns a registry of @c ICompositorSurface instances keyed by
 * @c FlutterPlatformViewIdentifier. On each frame the compositor dispatches
 * backing-store lifecycle and presentation calls into the matching surface.
 * Implementations must be callable from the rasterizer thread.
 */
class ICompositorSurface {
 public:
  virtual ~ICompositorSurface() = default;

  /**
   * @brief Fill @p store_out with a backing store for this surface's layer.
   *
   * The embedder retains ownership of any GPU resources referenced by the
   * returned store. The engine calls @c OnCollectBackingStore when the store
   * is no longer in use.
   *
   * @return true on success; false causes the engine to fall back to an
   *         engine-owned store for this frame.
   */
  virtual bool OnCreateBackingStore(const FlutterBackingStoreConfig* config,
                                    FlutterBackingStore* store_out) = 0;

  /**
   * @brief Retire a backing store previously produced by this surface.
   */
  virtual bool OnCollectBackingStore(const FlutterBackingStore* store) = 0;

  /**
   * @brief Present a frame for this surface.
   *
   * The compositor has already reconciled the Wayland subsurface Z-order
   * before this call. The implementation should draw into or swap its native
   * surface.
   */
  virtual bool OnPresent(const FlutterLayer* layer) = 0;

  /**
   * @brief The platform view identifier this surface is registered under.
   */
  [[nodiscard]] virtual FlutterPlatformViewIdentifier GetIdentifier() const = 0;

  /**
   * @brief Optional resize notification. Default is a no-op.
   */
  virtual void OnResize(int32_t /*width*/, int32_t /*height*/) {}
};
