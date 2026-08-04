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

#ifndef SHELL_DISPLAY_CONNECTOR_EDID_H_
#define SHELL_DISPLAY_CONNECTOR_EDID_H_

#include <cstdint>
#include <optional>

#include <drm-cxx/display/connector_info.hpp>

namespace homescreen {

/**
 * @brief Read and parse a connector's EDID.
 *
 * Fetches the connector's EDID property blob through @p drm_fd and parses it
 * with drm-cxx (libdisplay-info underneath), yielding the monitor's make,
 * model, serial and physical size.
 *
 * Shared rather than duplicated: the driver probe wants this to describe the
 * panel it is about to drive, and the output provider wants it so a view can
 * be matched to a monitor by serial rather than by port. Both need exactly the
 * same blob from exactly the same property.
 *
 * @param[in] drm_fd       an open DRM device; may be a lease, which the kernel
 *                         filters to the leased objects
 * @param[in] connector_id KMS connector object id
 * @return the parsed EDID, or nullopt when the connector exposes no EDID
 *         property, the blob is empty, or it does not parse
 *
 * @note nullopt is ordinary, not an error. A disconnected port has no EDID,
 *       and neither do many panels that are permanently wired to one -- DSI on
 *       an embedded board is the common case, and low-cost panels that do carry
 *       an EDID often omit the serial. Callers must treat every identity field
 *       as optional and fall back to the connector name.
 */
[[nodiscard]] std::optional<drm::display::ConnectorInfo> ProbeConnectorInfo(
    int drm_fd,
    uint32_t connector_id);

}  // namespace homescreen

#endif  // SHELL_DISPLAY_CONNECTOR_EDID_H_
