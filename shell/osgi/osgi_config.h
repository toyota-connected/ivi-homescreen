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

#include <filesystem>
#include <string>
#include <vector>

// Must precede any tomlplusplus include: configuration.h (reached through
// bundle_manifest.h) compiles toml++ with TOML_EXCEPTIONS 0, and that macro is
// only honored on the header's first inclusion. Including
// <tomlplusplus/toml.hpp> above this line would fix the library in throwing
// mode for this TU while the rest of the shell expects the result-returning
// API.
#include "bundle_manifest.h"

namespace ihs::osgi {

// The process-level [osgi] table.
struct OsgiConfig {
  // CPU to pin the Dart framework isolate's thread to. -1 = unpinned.
  int framework_core{-1};

  // [[osgi.bundles]], in declaration order. Bring-up order is decided by the
  // orchestrator from Priority, not by this vector's order.
  std::vector<BundleManifest> bundles;

  [[nodiscard]] bool empty() const { return bundles.empty(); }
};

// Parse the [osgi] table out of an already-parsed document @root.
//
// A document with no [osgi] table is not an error: @out is left empty and the
// function succeeds. That is the ENABLE_OSGI=ON, no-bundles-configured case,
// which must behave exactly like a stock build.
//
// Returns false on a malformed [osgi] table, having logged the specific
// problem. Callers should treat that as fatal: a bundle set that silently
// dropped an entry is worse than not starting.
// @root is taken by mutable reference only because the Configuration parsing
// helpers it delegates to take a non-const toml::table*; nothing is modified.
//
// @base_dir, when non-empty, is the directory holding the config file. Relative
// 'bundle' paths resolve against it, matching how Configuration::parse_config
// resolves them for [[view]]. Callers parsing an in-memory document with no
// backing file leave it empty, and relative paths then stay as written.
bool ParseOsgiTable(toml::table& root,
                    OsgiConfig& out,
                    const std::filesystem::path& base_dir = {});

// Read @config_toml_path and parse its [osgi] table. Returns false if the file
// is missing or does not parse.
bool LoadOsgiConfig(const std::string& config_toml_path, OsgiConfig& out);

}  // namespace ihs::osgi
