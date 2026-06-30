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

#include <optional>
#include <string>

namespace homescreen {

// Resolve which DRM card to scan out to / enumerate. The card numbering is not
// stable across systems or boots -- a single-GPU machine may expose only
// /dev/dri/card0 or only /dev/dri/card1 -- so a fixed default is wrong; this
// looks at the nodes that actually exist. Precedence:
//   1. An explicit, non-empty @p explicit_device (TOML / CLI) is returned
//      verbatim -- the operator's choice is never second-guessed.
//   2. Exactly one card node present -> that card (the unambiguous single-GPU
//      and headless-vkms cases), whatever its number, real or virtual.
//   3. Several cards -> the first with a connected, non-virtual connector, so a
//      headless vkms/Virtual node never shadows the real GPU.
//   4. Otherwise (several cards, none with a connected display) -> nullopt,
//      after logging each card and its connectors. The resolver refuses to
//      guess into a black screen; the caller fail-fasts and the operator
//      selects explicitly.
// The choice and the reasoning are always logged.
[[nodiscard]] std::optional<std::string> ResolveDrmDevice(
    const std::optional<std::string>& explicit_device);

}  // namespace homescreen
