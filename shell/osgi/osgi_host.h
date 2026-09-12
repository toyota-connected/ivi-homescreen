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

namespace ihs::osgi {

class BridgeRegistry;

// Install the IhsOsgiHost proc table, so the ihs_osgi_* surface in
// libihs_shared reaches @registry.
//
// This is what turns that surface from inert into working: the library holds no
// OSGi state at all and answers IHS_OSGI_ERR_UNAVAILABLE until a table is
// installed. Both transports end at the same registry, so which one a bundle
// uses changes nothing the orchestrator sees.
//
// Call once at OSGi bring-up, before any bundle engine is spawned, and pair it
// with UninstallOsgiHost. @registry must outlive the installation; in practice
// it is BridgeRegistry::Instance(), which outlives everything.
void InstallOsgiHost(BridgeRegistry& registry);

// Remove the table. Every ihs_osgi_* call then reports
// IHS_OSGI_ERR_UNAVAILABLE rather than reaching into a shell that is going
// away. Safe to call without a matching install.
void UninstallOsgiHost();

}  // namespace ihs::osgi
