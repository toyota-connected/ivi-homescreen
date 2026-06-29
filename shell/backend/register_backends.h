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
