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
#include <string_view>

// Standalone so the vsync coordinator -- which runs on the frame path -- can
// order bundles without pulling in Configuration and the whole TOML surface via
// bundle_manifest.h.

namespace ihs::osgi {

// When a bundle is brought up relative to the asio reactor, and the order in
// which its engine is handed a vsync baton.
//
// The enumerator order is the dispatch order: a lower value is served first, so
// a cluster or an alarm surface gets its baton before an infotainment view
// competes for the same vblank. Do not reorder.
//
//   kCritical   started (and waited on) before the reactor runs. A cluster or
//               an alarm surface must be ACTIVE before anything else competes
//               for the GPU, so the orchestrator blocks on each one up to
//               startup_timeout_ms.
//   kNormal     started after the reactor is running, staggered, so several
//               bundles do not contend for the same first-frame resources.
//   kBackground services with no view (CAN decoders, telemetry); started last
//               and never waited on.
enum class Priority : uint8_t {
  kCritical,
  kNormal,
  kBackground,
};

// Parses "critical" | "normal" | "background". Returns false (leaving @out
// untouched) for anything else, so callers can report the offending spelling.
bool ParsePriority(std::string_view text, Priority& out);

// The canonical spelling, for logs and round-tripping.
std::string_view PriorityName(Priority priority);

}  // namespace ihs::osgi
