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

#include "view/present_layer_sequencer.h"

#include <wayland-client-protocol.h>

PresentLayerSequencer::PresentLayerSequencer()
    : placer_([](wl_subsurface* subsurface, wl_surface* sibling) {
        wl_subsurface_place_above(subsurface, sibling);
      }),
      positioner_([](wl_subsurface* subsurface, int32_t x, int32_t y) {
        wl_subsurface_set_position(subsurface, x, y);
      }) {}

PresentLayerSequencer::PresentLayerSequencer(Placer placer,
                                             Positioner positioner)
    : placer_(std::move(placer)), positioner_(std::move(positioner)) {}

void PresentLayerSequencer::RegisterSubsurface(FlutterPlatformViewIdentifier id,
                                               wl_subsurface* subsurface,
                                               wl_surface* surface) {
  subsurface_map_[id] = Entry{subsurface, surface};
}

void PresentLayerSequencer::UnregisterSubsurface(
    FlutterPlatformViewIdentifier id) {
  subsurface_map_.erase(id);
}

void PresentLayerSequencer::Present(const FlutterLayer** layers,
                                    size_t count,
                                    wl_surface* root_surface,
                                    const MissingHandler& on_missing) {
  // Reuse the member vector's storage across frames: clear() preserves
  // capacity, and reserve() is a no-op once the high-water mark is set.
  // Eliminates the per-Present allocation+free pair.
  last_order_.clear();
  last_order_.reserve(count);

  wl_surface* sibling_surface = root_surface;

  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer || layer->type != kFlutterLayerContentTypePlatformView ||
        !layer->platform_view) {
      continue;
    }

    const FlutterPlatformViewIdentifier id = layer->platform_view->identifier;
    const auto it = subsurface_map_.find(id);
    if (it == subsurface_map_.end()) {
      if (on_missing) {
        on_missing(id);
      }
      continue;
    }

    const Entry& e = it->second;
    if (!e.subsurface || !e.surface) {
      continue;
    }

    if (sibling_surface && placer_) {
      placer_(e.subsurface, sibling_surface);
    }
    if (positioner_) {
      // layer->offset is in physical pixels; pass through as parent-local.
      // Buffer-scale conversion lives in plugin code if needed.
      positioner_(e.subsurface, static_cast<int32_t>(layer->offset.x),
                  static_cast<int32_t>(layer->offset.y));
    }

    sibling_surface = e.surface;
    last_order_.push_back(id);
  }
}
