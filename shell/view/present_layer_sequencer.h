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

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

#include <shell/platform/embedder/embedder.h>

struct wl_compositor;
struct wl_subsurface;
struct wl_surface;

/**
 * @brief Translates a FlutterLayer array into Wayland subsurface Z-order ops.
 *
 * On each frame the engine delivers an ordered @c FlutterLayer** array. The
 * sequencer reconciles the current @c wl_subsurface stack against the desired
 * order using @c wl_subsurface_place_above / @c wl_subsurface_place_below,
 * avoiding destroy/recreate of subsurfaces between frames.
 *
 * Platform-view subsurfaces are registered externally (once at platform view
 * creation). Unregistered identifiers encountered during @c Present are
 * skipped and reported via @c on_missing (for logging).
 */
class PresentLayerSequencer {
 public:
  using MissingHandler =
      std::function<void(FlutterPlatformViewIdentifier id)>;

  /**
   * @brief Hook invoked to raise @p subsurface immediately above @p sibling.
   * Defaults to @c wl_subsurface_place_above. Tests replace it with a probe.
   */
  using Placer =
      std::function<void(wl_subsurface* subsurface, wl_surface* sibling)>;

  PresentLayerSequencer();
  explicit PresentLayerSequencer(Placer placer);

  PresentLayerSequencer(const PresentLayerSequencer&) = delete;
  PresentLayerSequencer& operator=(const PresentLayerSequencer&) = delete;

  /**
   * @brief Register a Wayland subsurface for a platform view identifier.
   *
   * @p subsurface is the subsurface role; @p surface is the underlying
   * @c wl_surface referenced by Z-order ops. Must be called on the platform
   * thread before the engine asks to present a layer that refers to @p id.
   * Duplicate IDs replace the prior mapping.
   */
  void RegisterSubsurface(FlutterPlatformViewIdentifier id,
                          wl_subsurface* subsurface,
                          wl_surface* surface);

  /**
   * @brief Drop the mapping for @p id. The subsurface itself is not destroyed.
   */
  void UnregisterSubsurface(FlutterPlatformViewIdentifier id);

  /**
   * @brief Reconcile subsurface stack to match @p layers' order.
   *
   * @param layers      Ordered layer array from the engine.
   * @param count       Number of layers.
   * @param compositor  Unused today; reserved for future surface allocation.
   * @param root_surface Root @c wl_surface used as the reference for the
   *                    first platform-view layer.
   * @param on_missing  Optional callback invoked when a layer references a
   *                    platform view identifier that has not been registered.
   */
  void Present(const FlutterLayer** layers,
               size_t count,
               wl_compositor* compositor,
               wl_surface* root_surface,
               const MissingHandler& on_missing = {});

  /**
   * @brief Number of registered platform-view subsurfaces.
   */
  size_t RegisteredCount() const { return subsurface_map_.size(); }

  /**
   * @brief Current reconciled Z-order, bottom to top.
   *
   * Intended for tests; reflects the most recent @c Present call.
   */
  const std::vector<FlutterPlatformViewIdentifier>& LastOrder() const {
    return last_order_;
  }

 private:
  struct Entry {
    wl_subsurface* subsurface;
    wl_surface* surface;
  };

  std::unordered_map<FlutterPlatformViewIdentifier, Entry> subsurface_map_;
  std::vector<FlutterPlatformViewIdentifier> last_order_;
  Placer placer_;
};
