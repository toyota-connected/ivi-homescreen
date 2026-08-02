/*
 * Copyright 2020-2026 Toyota Connected North America
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

#include "display/connector_edid.h"

#include <string_view>
#include <utility>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/edid.hpp>

namespace homescreen {

std::optional<drm::display::ConnectorInfo> ProbeConnectorInfo(
    const int drm_fd,
    const uint32_t connector_id) {
  drmModeObjectProperties* props = drmModeObjectGetProperties(
      drm_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
  if (props == nullptr) {
    return std::nullopt;
  }
  uint32_t edid_blob_id = 0;
  for (uint32_t i = 0; i < props->count_props && edid_blob_id == 0; ++i) {
    drmModePropertyRes* prop = drmModeGetProperty(drm_fd, props->props[i]);
    if (prop != nullptr) {
      if (std::string_view(prop->name) == "EDID") {
        edid_blob_id = static_cast<uint32_t>(props->prop_values[i]);
      }
      drmModeFreeProperty(prop);
    }
  }
  drmModeFreeObjectProperties(props);
  if (edid_blob_id == 0) {
    return std::nullopt;
  }
  drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(drm_fd, edid_blob_id);
  if (blob == nullptr || blob->data == nullptr || blob->length == 0) {
    if (blob != nullptr) {
      drmModeFreePropertyBlob(blob);
    }
    return std::nullopt;
  }
  auto parsed = drm::display::parse_edid(drm::span<const uint8_t>(
      static_cast<const uint8_t*>(blob->data), blob->length));
  drmModeFreePropertyBlob(blob);
  if (!parsed) {
    return std::nullopt;
  }
  return {std::move(*parsed)};
}

}  // namespace homescreen
