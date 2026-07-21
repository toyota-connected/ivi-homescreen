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

struct FlutterDesktopEngineState;

// Installs the shell's ihs_shared platform-view host (ihs/platform_view_host.h)
// for @engine_state: an out-of-tree FFI plugin that links libihs_shared can
// then register a view factory via ihs_pv_register_factory, reuse the backend's
// Vulkan/EGL context, and submit frames — all routed to this engine's Backend,
// PlatformViewRegistry, and compositor. Call once at startup after the registry
// and backend exist; a no-op without BUILD_COMPOSITOR. Safe to call again to
// re-point the process-global host at a different engine (last wins).
void InstallPlatformViewHost(FlutterDesktopEngineState* engine_state);
