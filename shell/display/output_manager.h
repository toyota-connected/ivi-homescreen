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

#include "configuration/configuration.h"
#include "display/output.h"

class IDisplay;

namespace homescreen {

// Binds each view to a physical output by name, the same way for every backend:
// it reads the view's display IOutputProvider, applies the [view.output]
// OutputMatch through ResolveOutput, and hands back the connector / wl_output
// name the backend should scan out to. The match logic and the live-output set
// are the authority -- a Wayland compositor connection and a DRM card are both
// "one master domain, N named outputs", so the same path drives DRM connector
// selection and Wayland wl_output targeting.
class OutputManager {
 public:
  // Resolve the output name @p config's view should bind to, from @p display's
  // current outputs. @p family selects which OutputMatch field is compared
  // (drm_connector for kDrm, wl_name for kWayland). Returns:
  //   - the matched output name, when [view.output] names one that is present;
  //   - nullopt when the view sets no [view.output] constraint (the backend's
  //     own default / auto-pick stands) or the requested output is absent (the
  //     caller falls back and the miss is logged).
  // The decision is logged either way.
  // As ResolveForView, but distinguishing "no [view.output] constraint" from
  // "a constraint was set and nothing satisfied it". The plain overload
  // collapses both to nullopt, which is why a caller cannot currently park a
  // view whose named output is absent -- it cannot tell that case from a view
  // that never asked for one.
  [[nodiscard]] static OutputResolution ResolveForViewDetailed(
      const Configuration::Config& config,
      IDisplay* display,
      BackendFamily family);

  [[nodiscard]] static std::optional<std::string> ResolveForView(
      const Configuration::Config& config,
      IDisplay* display,
      BackendFamily family);
};

}  // namespace homescreen
