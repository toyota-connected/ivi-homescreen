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

#include <vector>

#include "backend/backend_registry.h"
#include "configuration/configuration.h"

// Registers every backend compiled into this binary into @p registry. Each
// backend lives behind its own BUILD_BACKEND_* gate, so a single-backend build
// registers exactly one descriptor.
void RegisterCompiledBackends(backend::BackendRegistry& registry);

// Ensures @p registry has an active backend. If one is already set (e.g. an
// explicit override), it is respected. Otherwise the compiled-in backends are
// registered and the active one is resolved from @p configs (the view.backend
// hint, or the sole backend). Returns false if none could be resolved (the
// caller fail-fasts). This lets App be self-contained: it resolves the backend
// on first display creation rather than relying on main() to have done so.
[[nodiscard]] bool EnsureActiveBackend(
    backend::BackendRegistry& registry,
    const std::vector<Configuration::Config>& configs);

// Resolves the backend registry key for a single view @p config: an exact
// [view] backend / --backend key when it names a compiled-in backend, else the
// sole compiled backend, else an environment heuristic (a live Wayland session
// -> wayland-*, otherwise KMS-direct -> drm-kms-* / software), honoring the
// legacy egl/vulkan renderer hint within the chosen family. Returns an empty
// string when nothing resolves. This is the per-view counterpart to the
// process-wide active backend: App uses it to place each view on the right
// device-context display, and FlutterView to build that view's Backend.
[[nodiscard]] std::string ResolveKeyForConfig(
    const backend::BackendRegistry& registry,
    const Configuration::Config& config);
