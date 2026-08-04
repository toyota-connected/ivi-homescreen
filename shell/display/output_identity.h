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

#ifndef SHELL_DISPLAY_OUTPUT_IDENTITY_H_
#define SHELL_DISPLAY_OUTPUT_IDENTITY_H_

#include <map>
#include <string>
#include <string_view>

namespace homescreen {

/**
 * @brief Integrator-assigned role names for a card's connectors.
 *
 * Maps connector name (as OutputInfo::name carries it, e.g. "DSI-1") to the
 * value of the connector's IHS_OUTPUT_NAME udev property.
 */
using OutputIdMap = std::map<std::string, std::string, std::less<>>;

/**
 * @brief Read the IHS_OUTPUT_NAME property of each connector on a card.
 *
 * Every key a view can be pinned with today is stable only for one board.
 * Measured across the two verification targets: the DSI panel filling the same
 * role is DSI-1 on a Raspberry Pi 4 and DSI-2 on a Pi 5, on different cards
 * with different drivers; neither panel publishes an EDID, so make/model/serial
 * are empty; and /dev/dri/cardN is assigned in probe order, so the numbers
 * differ too. Nothing the hardware offers names the *role*.
 *
 * A udev rule can, and udev models DRM connectors as devices
 * (DEVTYPE=drm_connector), so it can attach a property to one. This reads that
 * property back, letting a config say "the cluster display" and bind correctly
 * on either board. See docs for the rule.
 *
 * @param[in] card_sysname the card's sysfs name, e.g. "card1"; empty matches
 *                         connectors on any card, which is what a caller that
 *                         cannot identify its card (an fd it was handed, not a
 *                         node it opened) has to fall back to
 * @return connector name -> role name, for connectors that carry the property.
 *         A value is never empty, and a connector whose role is ambiguous is
 *         omitted rather than guessed (only reachable across cards, see
 *         card_sysname), so a caller may use any entry it finds as it stands.
 *         Empty when no rule is installed, which is the default and not an
 *         error: matching then falls through to the other tiers exactly as
 *         before.
 */
[[nodiscard]] OutputIdMap ReadOutputIds(std::string_view card_sysname);

}  // namespace homescreen

#endif  // SHELL_DISPLAY_OUTPUT_IDENTITY_H_
