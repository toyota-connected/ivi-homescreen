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

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <drm_fourcc.h>

#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include "view/presentation.h"

// What a CRTC's planes scan out, for telling a producer which formats would let
// its layer onto one (IhsPvCallbacks::scanout_hint). Built once per output.
class PlaneFormats {
 public:
  PlaneFormats() = default;

  // The planes of @p registry that can drive CRTC @p crtc_index, on the card
  // behind @p drm_fd. Overlays only when there are any: the primary carries
  // the Flutter UI, so a platform view's layer lands on an overlay. Cursor
  // planes are left out -- too small for a view.
  PlaneFormats(const drm::planes::PlaneRegistry& registry,
               const uint32_t crtc_index,
               const int drm_fd) {
    struct stat st{};
    if (drm_fd >= 0 && fstat(drm_fd, &st) == 0) {
      dev_ = static_cast<uint64_t>(st.st_rdev);
    }
    const auto& planes = registry.for_crtc(crtc_index);
    const bool have_overlay =
        std::any_of(planes.begin(), planes.end(), [](const auto* p) {
          return p->type == drm::planes::DRMPlaneType::OVERLAY;
        });
    for (const auto* p : planes) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR ||
          (have_overlay && p->type != drm::planes::DRMPlaneType::OVERLAY)) {
        continue;
      }
      tables_.push_back(p->format_table);
      for (const auto& fm : p->format_table.all()) {
        formats_.emplace_back(fm.fourcc, fm.modifier.value);
      }
    }
    std::sort(formats_.begin(), formats_.end());
    formats_.erase(std::unique(formats_.begin(), formats_.end()),
                   formats_.end());
  }

  // Whether some plane scans out @p fourcc with @p modifier. An unknown
  // modifier (DRM_FORMAT_MOD_INVALID: the driver infers it) fits wherever the
  // fourcc does. True when there are no planes to ask: then there is no
  // better format to hint either.
  [[nodiscard]] bool Scans(const uint32_t fourcc,
                           const uint64_t modifier) const {
    if (tables_.empty()) {
      return true;
    }
    return std::any_of(tables_.begin(), tables_.end(),
                       [fourcc, modifier](const drm::fmt::FormatTable& t) {
                         if (modifier == DRM_FORMAT_MOD_INVALID) {
                           return !t.modifiers_for(fourcc).empty();
                         }
                         return t.supports(fourcc,
                                           drm::fmt::Modifier{modifier});
                       });
  }

  // What those planes scan out, sorted and without repeats.
  [[nodiscard]] const std::vector<FormatModifierPair>& formats() const {
    return formats_;
  }
  // The card's dev_t, for a producer to allocate against.
  [[nodiscard]] uint64_t dev() const { return dev_; }

  // Tell @p sink what layer @p layer_id, a frame of @p fourcc / @p modifier,
  // needs to reach a plane: nothing when it already could, else the formats
  // that would.
  void Hint(IPresentationSink* sink,
            const uint32_t layer_id,
            const uint32_t fourcc,
            const uint64_t modifier) const {
    if (sink == nullptr) {
      return;
    }
    static const std::vector<FormatModifierPair> kFits;
    sink->OnScanoutHint(layer_id, dev_,
                        Scans(fourcc, modifier) ? kFits : formats_);
  }

 private:
  uint64_t dev_{0};
  std::vector<drm::fmt::FormatTable> tables_;
  std::vector<FormatModifierPair> formats_;
};
