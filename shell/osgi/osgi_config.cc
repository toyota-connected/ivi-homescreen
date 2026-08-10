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

#include "osgi_config.h"

#include <sched.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "cpu_affinity.h"
#include "logging/logging.h"

namespace ihs::osgi {

namespace {

// A startup barrier longer than this is a typo, not an intent: the orchestrator
// blocks the reactor on it.
constexpr int kMaxTimeoutMs = 3600000;  // one hour

// Validate a core index against the CPUs this process may actually be pinned
// to. -1 means unpinned and is always accepted.
//
// Rejecting here rather than at pin time is deliberate. pthread_setaffinity_np
// against an unavailable core fails at startup, and a critical bundle that
// silently runs unpinned has lost exactly the scheduling guarantee its
// cpu_core was written to obtain -- the failure would show up as jitter on a
// cluster or an alarm surface, far from its cause.
bool ValidateCpuCore(const int core,
                     const std::string_view what,
                     const std::string_view who) {
  if (core == -1 || IsCpuAvailable(core)) {
    return true;
  }
  if (CpuAffinityKnown()) {
    ihs::log::critical(
        "[osgi] {}{} is {}, which this process may not be pinned to; "
        "available CPUs: {}",
        who, what, core, AvailableCpuList());
  } else {
    ihs::log::critical(
        "[osgi] {}{} is {}; expected -1 (unpinned) or a core "
        "index",
        who, what, core);
  }
  return false;
}

// Read a TOML integer, rejecting anything outside [min, max].
//
// TOML integers are int64_t. Narrowing with value<int>() yields an empty
// optional when the value does not fit, so calling .value() on it would throw
// std::bad_optional_access on a config that is merely malformed -- a config
// typo must be a diagnosed error, never a crash. Read the declared width and
// range-check here instead.
bool ReadBoundedInt(const toml::node_view<toml::node>& node,
                    const int64_t min,
                    const int64_t max,
                    int& out) {
  const std::optional<int64_t> raw = node.value<int64_t>();
  if (!raw.has_value() || *raw < min || *raw > max) {
    return false;
  }
  out = static_cast<int>(*raw);
  return true;
}

// Pull one [[osgi.bundles]] entry into @manifest. Returns false having logged
// the reason; @index is only for the message, since a malformed entry may not
// have a usable symbolic name to name itself with.
//
// @base_dir, when non-empty, is the directory of the config file: a relative
// 'bundle' path resolves against it, matching how [[view]] entries are resolved
// in Configuration::parse_config. Without this the same relative path would
// mean different things in the [[view]] and [[osgi.bundles]] halves of one
// file.
bool ParseBundle(toml::table& tbl,
                 const size_t index,
                 const std::filesystem::path& base_dir,
                 BundleManifest& manifest) {
  if (!tbl.at_path("symbolic_name").is_string()) {
    ihs::log::critical(
        "[osgi] [[osgi.bundles]] #{} is missing a 'symbolic_name' string",
        index);
    return false;
  }
  manifest.symbolic_name =
      tbl.at_path("symbolic_name").as_string()->value_or("");
  if (manifest.symbolic_name.empty()) {
    ihs::log::critical(
        "[osgi] [[osgi.bundles]] #{} has an empty 'symbolic_name'", index);
    return false;
  }

  // The bundle directory. Named 'bundle' to match [[view]], where the same key
  // carries the same meaning.
  if (!tbl.at_path("bundle").is_string()) {
    ihs::log::critical("[osgi] bundle '{}' is missing a 'bundle' path",
                       manifest.symbolic_name);
    return false;
  }
  const std::string bundle = tbl.at_path("bundle").as_string()->value_or("");
  if (bundle.empty()) {
    ihs::log::critical("[osgi] bundle '{}' has an empty 'bundle' path",
                       manifest.symbolic_name);
    return false;
  }
  if (std::filesystem::path bundle_path(bundle);
      bundle_path.is_relative() && !base_dir.empty()) {
    manifest.config.view.bundle_path = (base_dir / bundle_path).string();
  } else {
    manifest.config.view.bundle_path = bundle;
  }

  if (const auto node = tbl.at_path("priority"); node.is_string()) {
    const std::string text = node.as_string()->value_or("");
    if (!ParsePriority(text, manifest.priority)) {
      ihs::log::critical(
          "[osgi] bundle '{}' has priority '{}'; expected "
          "critical|normal|background",
          manifest.symbolic_name, text);
      return false;
    }
  }

  if (tbl.contains("cpu_core")) {
    if (!ReadBoundedInt(tbl.at_path("cpu_core"), -1, CPU_SETSIZE - 1,
                        manifest.cpu_core)) {
      ihs::log::critical(
          "[osgi] bundle '{}' has an out-of-range cpu_core; expected -1 "
          "(unpinned) or a core index below {}",
          manifest.symbolic_name, CPU_SETSIZE);
      return false;
    }
    if (!ValidateCpuCore(manifest.cpu_core, "cpu_core",
                         "bundle '" + manifest.symbolic_name + "' ")) {
      return false;
    }
  }

  if (tbl.contains("startup_timeout_ms") &&
      !ReadBoundedInt(tbl.at_path("startup_timeout_ms"), 1, kMaxTimeoutMs,
                      manifest.startup_timeout_ms)) {
    ihs::log::critical(
        "[osgi] bundle '{}' has an invalid startup_timeout_ms; expected a "
        "positive millisecond count up to {}",
        manifest.symbolic_name, kMaxTimeoutMs);
    return false;
  }

  // Everything else -- backend, output, args, shell/window, geometry -- is a
  // [view] key with [view] semantics, so it goes through the same parser rather
  // than being reimplemented here. Keys it does not recognize (the OSGi ones
  // handled above) are ignored.
  Configuration::get_view_parameters(&tbl, manifest.config);
  return true;
}

}  // namespace

bool ParseOsgiTable(toml::table& root,
                    OsgiConfig& out,
                    const std::filesystem::path& base_dir) {
  const auto osgi_node = root.at_path("osgi");
  if (!osgi_node) {
    // No [osgi] table. A stock config under an ENABLE_OSGI build.
    return true;
  }
  if (!osgi_node.is_table()) {
    ihs::log::critical("[osgi] 'osgi' must be a table");
    return false;
  }

  if (osgi_node.as_table()->contains("framework_core")) {
    if (!ReadBoundedInt(root.at_path("osgi.framework_core"), -1,
                        CPU_SETSIZE - 1, out.framework_core)) {
      ihs::log::critical(
          "[osgi] out-of-range framework_core; expected -1 (unpinned) or a "
          "core index below {}",
          CPU_SETSIZE);
      return false;
    }
    if (!ValidateCpuCore(out.framework_core, "framework_core", "")) {
      return false;
    }
  }

  const auto bundles_node = root.at_path("osgi.bundles");
  if (!bundles_node) {
    ihs::log::warn("[osgi] [osgi] present with no [[osgi.bundles]] entries");
    return true;
  }
  if (!bundles_node.is_array()) {
    ihs::log::critical(
        "[osgi] 'osgi.bundles' must be an array of tables ([[osgi.bundles]])");
    return false;
  }

  // Two identities must be unique across the process: the symbolic name (OSGi
  // requires it) and, for ivi-shell views, the ivi_surface_id -- a collision
  // there silently hands two bundles the same compositor surface.
  std::unordered_set<std::string> seen_names;
  std::unordered_map<uint32_t, std::string> seen_surface_ids;

  // A BundleManifest embeds a whole Configuration::Config (dozens of strings,
  // vectors and optionals), so growth reallocation is not free. The entry count
  // is known up front.
  // Not const: Configuration::get_view_parameters takes a mutable toml::table*,
  // so the elements have to stay non-const all the way down.
  auto* bundles = bundles_node.as_array();
  out.bundles.reserve(out.bundles.size() + bundles->size());

  size_t index = 0;
  for (auto&& element : *bundles) {
    auto* tbl = element.as_table();
    if (tbl == nullptr) {
      ihs::log::critical("[osgi] [[osgi.bundles]] #{} is not a table", index);
      return false;
    }

    BundleManifest manifest;
    if (!ParseBundle(*tbl, index, base_dir, manifest)) {
      return false;
    }

    if (!seen_names.insert(manifest.symbolic_name).second) {
      ihs::log::critical("[osgi] duplicate symbolic_name '{}'",
                         manifest.symbolic_name);
      return false;
    }

    if (manifest.config.view.ivi_surface_id.has_value()) {
      const uint32_t surface_id = *manifest.config.view.ivi_surface_id;
      const auto [it, inserted] =
          seen_surface_ids.emplace(surface_id, manifest.symbolic_name);
      if (!inserted) {
        ihs::log::critical(
            "[osgi] bundles '{}' and '{}' both request ivi-shell surface_id {}",
            it->second, manifest.symbolic_name, surface_id);
        return false;
      }
    }

    out.bundles.push_back(std::move(manifest));
    ++index;
  }

  return true;
}

bool LoadOsgiConfig(const std::string& config_toml_path, OsgiConfig& out) {
  if (!std::filesystem::exists(config_toml_path)) {
    ihs::log::critical("[osgi] config file not found: {}", config_toml_path);
    return false;
  }

  // configuration.h compiles tomlplusplus with TOML_EXCEPTIONS 0, so
  // parse_file returns a result to test rather than throwing.
  auto result = toml::parse_file(config_toml_path);
  if (!result) {
    ihs::log::critical("[osgi] TOML parsing failed: {} — {}", config_toml_path,
                       result.error().description());
    return false;
  }

  // Relative bundle paths resolve against the config file's directory, exactly
  // as Configuration::parse_config resolves them for [[view]].
  return ParseOsgiTable(result.table(), out,
                        std::filesystem::path(config_toml_path).parent_path());
}

}  // namespace ihs::osgi
