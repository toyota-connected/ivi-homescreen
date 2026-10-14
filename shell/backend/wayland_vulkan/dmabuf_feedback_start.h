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

#include <wayland-client.h>

// Include after the generated linux_dmabuf_client.hpp and wl/linux_dmabuf.hpp.
#include <wl/dmabuf_feedback.hpp>

namespace wl_vulkan {

// Where the compositor's dma-buf modifiers come from.
enum class DmabufFeedbackSource {
  // Not bound, or bound below v3: no modifiers are known.
  kNone,
  // v4+: per-surface feedback, with scanout tranches and the main device.
  kSurface,
  // v3: the bind's format/modifier events, as one tranche with no scanout
  // flag and no device. A v4+ compositor no longer sends them.
  kFormatEvents,
};

// Binds @p feedback (recorded already) and starts it for @p surface, then
// drains the first advertisement with a roundtrip so Current() holds it.
// Returns where the modifiers came from; kNone leaves Current() empty.
template <typename App>
DmabufFeedbackSource StartDmabufFeedback(wl::DmabufFeedback<App>& feedback,
                                         wl::CRegistry& registry,
                                         App* app,
                                         wl_display* display,
                                         wl_proxy* surface) {
  if (!feedback.Bind(registry, app)) {
    return DmabufFeedbackSource::kNone;
  }
  if (feedback.StartSurface(display, surface)) {
    wl_display_roundtrip(display);
    return DmabufFeedbackSource::kSurface;
  }
  if (feedback.BoundVersion() == 3) {
    wl_display_roundtrip(display);
    feedback.CommitLegacy();
    return DmabufFeedbackSource::kFormatEvents;
  }
  return DmabufFeedbackSource::kNone;
}

}  // namespace wl_vulkan
