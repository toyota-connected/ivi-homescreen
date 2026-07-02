// Copyright 2022 Toyota Connected North America
// @copyright Copyright (c) 2022 Woven Alpha, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "configuration.h"
#include "logging/logging.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>

#include "config/common.h"
#include "cxxopts/include/cxxopts.hpp"
#include "utils.h"

namespace {
// Resolve the view tables of a parsed document. Accepts both the
// array-of-tables form `[[view]]` (one element per view) and a singular
// `[view]` table (treated as a one-element list) so per-bundle files can keep
// the terser singular form.
std::vector<toml::table*> collect_view_tables(toml::table* root) {
  std::vector<toml::table*> views;
  auto node = root->at_path("view");
  if (auto* arr = node.as_array()) {
    for (auto&& el : *arr) {
      if (auto* t = el.as_table()) {
        views.emplace_back(t);
      }
    }
  } else if (auto* t = node.as_table()) {
    views.emplace_back(t);
  }
  return views;
}
}  // namespace

// Overlays the [global] (process-level) table onto |instance|, copying each key
// only when present and of the expected type so unset keys preserve any
// defaults or command-line values already held by |instance|.
void Configuration::get_global_parameters(toml::table* root, Config& instance) {
  if (root->at_path("global.app_id").is_string()) {
    instance.app_id = root->at_path("global.app_id").as_string()->value_or("");
  }
  if (root->at_path("global.cursor_theme").is_string()) {
    instance.cursor_theme =
        root->at_path("global.cursor_theme").as_string()->value_or("");
  }
  if (root->at_path("global.disable_cursor").is_boolean()) {
    instance.disable_cursor =
        root->at_path("global.disable_cursor").value<bool>().value();
  }
  if (root->at_path("global.wayland_event_mask").is_string()) {
    instance.wayland_event_mask =
        root->at_path("global.wayland_event_mask").as_string()->value_or("");
  }
  if (root->at_path("global.debug_backend").is_boolean()) {
    instance.debug_backend =
        root->at_path("global.debug_backend").value<bool>().value();
  }
}

namespace {
// Fill an OutputMatch from one output table (keys relative to that table). Used
// for both a single [view.output] table and each element of a [[view.output]]
// array.
void ParseOutputMatch(const toml::table& t, homescreen::OutputMatch& out) {
  if (t["name"].is_string()) {
    // Convenience: `name` fills both the wl_output and DRM connector fields so
    // one config works on either backend.
    const std::string name = t["name"].as_string()->value_or("");
    out.wl_name = name;
    out.drm_connector = name;
  }
  if (t["wl_name"].is_string()) {
    out.wl_name = t["wl_name"].as_string()->value_or("");
  }
  if (t["drm_connector"].is_string()) {
    out.drm_connector = t["drm_connector"].as_string()->value_or("");
  }
  if (t["serial"].is_string()) {
    out.edid_serial = t["serial"].as_string()->value_or("");
  }
  if (t["index"].is_integer()) {
    out.index = t["index"].value<uint32_t>().value();
  }
  if (t["preload"].is_boolean()) {
    out.preload = t["preload"].value<bool>().value();
  }
  if (t["on_disconnect"].is_string()) {
    const std::string policy = t["on_disconnect"].as_string()->value_or("");
    out.on_disconnect = (policy == "teardown")
                            ? homescreen::OutputMatch::OnDisconnect::kTeardown
                            : homescreen::OutputMatch::OnDisconnect::kSuspend;
  }
  // Position of this output in the combined desktop space (input layout).
  if (t["x"].is_integer()) {
    out.layout_x = t["x"].value<int32_t>();
  }
  if (t["y"].is_integer()) {
    out.layout_y = t["y"].value<int32_t>();
  }
  // Touch panel bonded to this output (libinput device-name substring).
  if (t["touch_device"].is_string()) {
    out.touch_device = t["touch_device"].as_string()->value_or("");
  }
}
}  // namespace

// Overlays a single [[view]] table onto |instance|. Paths are relative to the
// view table, so AGL shell knobs live under shell.window[.activation_area] and
// backend knobs under backend[.drm], each ignored unless present.
void Configuration::get_view_parameters(toml::table* v, Config& instance) {
  // Plain view geometry / Flutter settings.
  if (v->at_path("output_index").is_integer()) {
    instance.view.wl_output_index =
        v->at_path("output_index").value<uint32_t>().value();
  }
  // [view.output] — pin this view to a physical output (see
  // homescreen::ResolveOutput). `name` is the cross-backend convenience: it
  // fills both the wl_output and DRM connector fields so one config works on
  // either backend. wl_name / drm_connector override per backend; serial is a
  // DRM-only EDID refinement.
  // A single [view.output] table binds one output. An array [[view.output]]
  // binds one engine to several outputs: the first entry is the
  // primary (implicit view / vsync driver), the rest become added views on
  // their own connectors.
  if (const auto node = v->at_path("output"); node.is_array()) {
    bool first = true;
    for (auto&& element : *node.as_array()) {
      const auto* t = element.as_table();
      if (t == nullptr) {
        continue;
      }
      if (first) {
        ParseOutputMatch(*t, instance.view.output);
        first = false;
      } else {
        homescreen::OutputMatch extra;
        ParseOutputMatch(*t, extra);
        instance.view.additional_outputs.push_back(std::move(extra));
      }
    }
  } else if (const auto* t = node.as_table()) {
    ParseOutputMatch(*t, instance.view.output);
  }
  if (v->at_path("width").is_integer()) {
    instance.view.width = v->at_path("width").value<uint32_t>().value();
  }
  if (v->at_path("height").is_integer()) {
    instance.view.height = v->at_path("height").value<uint32_t>().value();
  }
  if (v->at_path("pixel_ratio").is_floating_point()) {
    instance.view.pixel_ratio =
        v->at_path("pixel_ratio").value<double>().value();
  }
  if (v->at_path("accessibility_features").is_integer()) {
    instance.view.accessibility_features =
        v->at_path("accessibility_features").value<int32_t>().value();
  }
  // [view.args] — engine -> command_line_argv, dart -> dart_entrypoint_argv.
  if (v->at_path("args.engine").is_array()) {
    const auto engine_args = v->at_path("args.engine").as_array();
    for (auto& arg : *engine_args) {
      instance.view.engine_args.emplace_back(arg.as_string()->value_or(""));
    }
  }
  if (v->at_path("args.dart").is_array()) {
    const auto dart_args = v->at_path("args.dart").as_array();
    for (auto& arg : *dart_args) {
      instance.view.dart_args.emplace_back(arg.as_string()->value_or(""));
    }
  }
  if (v->at_path("fullscreen").is_boolean()) {
    instance.view.fullscreen = v->at_path("fullscreen").value<bool>().value();
  }

  // [view.engine] — engine threading knobs. merge_render_platform runs the
  // raster thread on the platform thread (one task-runner identifier for both).
  if (v->at_path("engine.merge_render_platform").is_boolean()) {
    instance.view.merge_render_platform =
        v->at_path("engine.merge_render_platform").value<bool>().value();
  }

  // [view.shell] — shell selector + shell-specific knobs (AGL window role /
  // activation area; ivi surface id).
  if (v->at_path("shell.type").is_string()) {
    instance.view.shell = v->at_path("shell.type").as_string()->value_or("");
  }
  // ivi-shell addresses surfaces by numeric id (ivi-only).
  if (v->at_path("shell.surface_id").is_integer()) {
    instance.view.ivi_surface_id =
        v->at_path("shell.surface_id").value<uint32_t>().value();
  }
  if (v->at_path("shell.window.type").is_string()) {
    instance.view.window_type =
        v->at_path("shell.window.type").as_string()->value_or("");
  }
  if (v->at_path("shell.window.activation_area.x").is_integer()) {
    instance.view.activation_area_x =
        v->at_path("shell.window.activation_area.x").value<uint32_t>().value();
  }
  if (v->at_path("shell.window.activation_area.y").is_integer()) {
    instance.view.activation_area_y =
        v->at_path("shell.window.activation_area.y").value<uint32_t>().value();
  }
  if (v->at_path("shell.window.activation_area.width").is_integer()) {
    instance.view.activation_area_width =
        v->at_path("shell.window.activation_area.width")
            .value<uint32_t>()
            .value();
  }
  if (v->at_path("shell.window.activation_area.height").is_integer()) {
    instance.view.activation_area_height =
        v->at_path("shell.window.activation_area.height")
            .value<uint32_t>()
            .value();
  }

  // [view.backend] — backend selector + DRM-only knobs (see configuration.h).
  if (v->at_path("backend.type").is_string()) {
    instance.view.backend =
        v->at_path("backend.type").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.device").is_string()) {
    instance.view.drm_device =
        v->at_path("backend.drm.device").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.connector").is_string()) {
    instance.view.drm_connector =
        v->at_path("backend.drm.connector").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.mode").is_string()) {
    instance.view.drm_mode =
        v->at_path("backend.drm.mode").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.rotation").is_integer()) {
    instance.view.drm_rotation =
        v->at_path("backend.drm.rotation").value<int>().value();
  }
  if (v->at_path("backend.drm.compositor").is_string()) {
    instance.view.drm_compositor =
        v->at_path("backend.drm.compositor").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.modeset").is_string()) {
    instance.view.drm_modeset =
        v->at_path("backend.drm.modeset").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.allow_nonblock_modeset").is_string()) {
    instance.view.drm_allow_nonblock_modeset =
        v->at_path("backend.drm.allow_nonblock_modeset")
            .as_string()
            ->value_or("");
  }
  if (v->at_path("backend.drm.primary_format").is_string()) {
    instance.view.drm_primary_format =
        v->at_path("backend.drm.primary_format").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.overlay_planes").is_string()) {
    instance.view.drm_overlay_planes =
        v->at_path("backend.drm.overlay_planes").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.explicit_sync").is_string()) {
    instance.view.drm_explicit_sync =
        v->at_path("backend.drm.explicit_sync").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.async_flip").is_string()) {
    instance.view.drm_async_flip =
        v->at_path("backend.drm.async_flip").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.stage_cursor").is_string()) {
    instance.view.drm_stage_cursor =
        v->at_path("backend.drm.stage_cursor").as_string()->value_or("");
  }
  if (v->at_path("backend.drm.no_seat").is_boolean()) {
    instance.view.drm_no_seat =
        v->at_path("backend.drm.no_seat").value<bool>().value();
  }
  if (v->at_path("backend.drm.input_transforms").is_array()) {
    const auto arr = v->at_path("backend.drm.input_transforms").as_array();
    instance.view.drm_input_transforms.clear();
    for (auto& t : *arr) {
      instance.view.drm_input_transforms.emplace_back(
          t.as_string()->value_or(""));
    }
  }
}

void Configuration::get_toml_config(const char* config_toml_path,
                                    Config& instance) {
  if (!std::filesystem::exists(config_toml_path)) {
    return;
  }

  auto result = toml::parse_file(config_toml_path);
  if (!result) {
    spdlog::error("TOML parsing failed: {}", config_toml_path);
    exit(EXIT_FAILURE);
  }

  auto tbl = result.table();
  get_global_parameters(&tbl, instance);
  // A per-bundle config.toml describes a single view; apply the first (or only)
  // [[view]]/[view] table.
  const auto views = collect_view_tables(&tbl);
  if (!views.empty()) {
    get_view_parameters(views.front(), instance);
  }
}

void Configuration::get_cli_override(const std::string& bundle_path,
                                     Config& instance,
                                     const Config& cli) {
  instance.view.bundle_path = bundle_path;

  if (!cli.app_id.empty()) {
    instance.app_id = cli.app_id;
  }
  if (!cli.cursor_theme.empty()) {
    instance.cursor_theme = cli.cursor_theme;
  }
  if (cli.disable_cursor.has_value()) {
    instance.disable_cursor = cli.disable_cursor.value();
  }
  if (!cli.wayland_event_mask.empty()) {
    instance.wayland_event_mask = cli.wayland_event_mask;
  }
  if (cli.debug_backend.has_value()) {
    instance.debug_backend = cli.debug_backend.value();
  }
  if (!cli.view.engine_args.empty()) {
    for (auto const& arg : cli.view.engine_args) {
      instance.view.engine_args.emplace_back(arg);
    }
  }
  if (!cli.view.dart_args.empty()) {
    for (auto const& arg : cli.view.dart_args) {
      instance.view.dart_args.emplace_back(arg);
    }
  }
  if (!cli.view.window_type.empty()) {
    instance.view.window_type = cli.view.window_type;
  }
  if (cli.view.shell.has_value()) {
    instance.view.shell = cli.view.shell.value();
  }
  if (cli.view.backend.has_value()) {
    instance.view.backend = cli.view.backend.value();
  }
  if (cli.view.wl_output_index.has_value()) {
    instance.view.wl_output_index = cli.view.wl_output_index.value();
  }
  if (cli.view.accessibility_features.has_value()) {
    instance.view.accessibility_features =
        mask_accessibility_features(cli.view.accessibility_features.value());
  }
  if (cli.view.width.has_value()) {
    instance.view.width = cli.view.width.value();
  }
  if (cli.view.height.has_value()) {
    instance.view.height = cli.view.height.value();
  }
  if (cli.view.pixel_ratio.has_value()) {
    instance.view.pixel_ratio = cli.view.pixel_ratio.value();
  }
  if (cli.view.ivi_surface_id.has_value()) {
    instance.view.ivi_surface_id = cli.view.ivi_surface_id.value();
  }
  if (cli.view.fullscreen.has_value()) {
    instance.view.fullscreen = cli.view.fullscreen.value();
  }
  if (cli.view.drm_device.has_value()) {
    instance.view.drm_device = cli.view.drm_device.value();
  }
  if (cli.view.drm_connector.has_value()) {
    instance.view.drm_connector = cli.view.drm_connector.value();
  }
  if (cli.view.drm_mode.has_value()) {
    instance.view.drm_mode = cli.view.drm_mode.value();
  }
  if (cli.view.drm_rotation.has_value()) {
    instance.view.drm_rotation = cli.view.drm_rotation.value();
  }
  if (!cli.view.drm_input_transforms.empty()) {
    instance.view.drm_input_transforms = cli.view.drm_input_transforms;
  }
  if (cli.view.drm_compositor.has_value()) {
    instance.view.drm_compositor = cli.view.drm_compositor.value();
  }
  if (cli.view.drm_modeset.has_value()) {
    instance.view.drm_modeset = cli.view.drm_modeset.value();
  }
  if (cli.view.drm_allow_nonblock_modeset.has_value()) {
    instance.view.drm_allow_nonblock_modeset =
        cli.view.drm_allow_nonblock_modeset.value();
  }
  if (cli.view.drm_primary_format.has_value()) {
    instance.view.drm_primary_format = cli.view.drm_primary_format.value();
  }
  if (cli.view.drm_overlay_planes.has_value()) {
    instance.view.drm_overlay_planes = cli.view.drm_overlay_planes.value();
  }
  if (cli.view.drm_explicit_sync.has_value()) {
    instance.view.drm_explicit_sync = cli.view.drm_explicit_sync.value();
  }
  if (cli.view.drm_async_flip.has_value()) {
    instance.view.drm_async_flip = cli.view.drm_async_flip.value();
  }
  if (cli.view.drm_stage_cursor.has_value()) {
    instance.view.drm_stage_cursor = cli.view.drm_stage_cursor.value();
  }
  if (cli.view.drm_no_seat.has_value()) {
    instance.view.drm_no_seat = cli.view.drm_no_seat.value();
  }
}

std::vector<Configuration::Config> Configuration::parse_config(
    const Config& cli_config) {
  // One descriptor per view: its bundle directory plus, when driven by a master
  // --config file, the [[view]] table that overrides the bundle's own config.
  struct ViewDescriptor {
    std::string bundle_path;
    toml::table* master_view = nullptr;  // null in legacy (-b) mode
  };
  std::vector<ViewDescriptor> descriptors;

  // Parse the master --config document once and keep it alive for the whole
  // function so the table pointers handed to get_*_parameters stay valid.
  toml::parse_result master_doc;
  toml::table* master_root = nullptr;
  if (cli_config.config_file && !cli_config.config_file->empty()) {
    const auto& path = *cli_config.config_file;
    if (!std::filesystem::exists(path)) {
      spdlog::critical("--config file not found: {}", path);
      exit(EXIT_FAILURE);
    }
    master_doc = toml::parse_file(path);
    if (!master_doc) {
      spdlog::critical("TOML parsing failed: {} — {}", path,
                       master_doc.error().description());
      exit(EXIT_FAILURE);
    }
    master_root = &master_doc.table();

    // Resolve each view's bundle, relative paths against the file's directory.
    const std::filesystem::path base =
        std::filesystem::path(path).parent_path();
    for (auto* v : collect_view_tables(master_root)) {
      std::string bundle;
      if (v->at_path("bundle").is_string()) {
        bundle = v->at_path("bundle").as_string()->value_or("");
      }
      if (bundle.empty()) {
        spdlog::critical("--config: every [[view]] requires a 'bundle' path");
        exit(EXIT_FAILURE);
      }
      std::filesystem::path bp(bundle);
      if (bp.is_relative()) {
        bp = base / bp;
      }
      descriptors.push_back({bp.string(), v});
    }
    if (!cli_config.bundle_paths.empty()) {
      spdlog::warn("--config provided; ignoring -b/--bundle argument(s)");
    }
  } else {
    for (const auto& bundle_path : cli_config.bundle_paths) {
      descriptors.push_back({bundle_path, nullptr});
    }
  }

  std::vector<Config> res;
  res.reserve(descriptors.size());

  for (const auto& d : descriptors) {
    Config cfg{};
    // Propagate the master path so the crash handler can source [sentry] from
    // it (process-level), not from the first bundle.
    cfg.config_file = cli_config.config_file;

    // 1. The bundle's own config.toml (base layer): its [global] + single view.
    const auto config_toml_path = d.bundle_path + "/" + kViewConfigToml;
    get_toml_config(config_toml_path.c_str(), cfg);

    // 2. The master --config overrides: process [global] then this view entry.
    if (master_root) {
      get_global_parameters(master_root, cfg);
      if (d.master_view) {
        get_view_parameters(d.master_view, cfg);
      }
    }

    // 3. CLI flags win.
    get_cli_override(d.bundle_path, cfg, cli_config);

    if (cfg.view.window_type.empty()) {
      // The AGL shell requires a real surface role (BG / PANEL_*); NORMAL has
      // no AGL window-management meaning, so an unset type would never be
      // shown. Default an AGL view to the background role; every other shell
      // keeps NORMAL.
      cfg.view.window_type =
          (cfg.view.shell.value_or("") == "agl") ? "BG" : "NORMAL";
    }
    if (cfg.view.width == 0) {
      cfg.view.width = kDefaultViewWidth;
    }
    if (cfg.view.height == 0) {
      cfg.view.height = kDefaultViewHeight;
    }
    if (cfg.view.pixel_ratio == 0) {
      cfg.view.pixel_ratio = kDefaultPixelRatio;
    }
    if (cfg.app_id.empty()) {
      cfg.app_id = kDefaultAppId;
    }

    // An omitted/zero activation area covers the full view extent. TOML has no
    // value references, so "same as the view" is resolved here rather than in
    // the file. x/y already default to 0.
    const uint32_t view_w = cfg.view.width.value_or(kDefaultViewWidth);
    const uint32_t view_h = cfg.view.height.value_or(kDefaultViewHeight);
    if (cfg.view.activation_area_width == 0) {
      cfg.view.activation_area_width = view_w;
    }
    if (cfg.view.activation_area_height == 0) {
      cfg.view.activation_area_height = view_h;
    }

    res.emplace_back(cfg);
  }

  return res;
}

void Configuration::PrintConfig(const Config& config) {
  spdlog::info("{} @ {}", kGitBranch, kGitCommitHash);

  spdlog::info("**********");
  spdlog::info("* Global *");
  spdlog::info("**********");
  if (!config.app_id.empty()) {
    spdlog::info("Application Id: .......... {}", config.app_id);
  }
  if (!config.cursor_theme.empty()) {
    spdlog::info("Cursor Theme: ............ {}", config.cursor_theme);
  }
  spdlog::info("Disable Cursor: .......... {}",
               (config.disable_cursor.value_or(false) ? "true" : "false"));
  if (!config.wayland_event_mask.empty()) {
    spdlog::info("Wayland Event Mask: ...... {}", config.wayland_event_mask);
  }
  spdlog::info("Debug Shell: ........... {}",
               (config.debug_backend.value_or(false) ? "true" : "false"));
  spdlog::info("********");
  spdlog::info("* View *");
  spdlog::info("********");
  if (!config.view.engine_args.empty()) {
    spdlog::info("Engine Args:");
    for (auto const& arg : config.view.engine_args) {
      spdlog::info(arg);
    }
  }
  if (!config.view.dart_args.empty()) {
    spdlog::info("Dart Entrypoint Args:");
    for (auto const& arg : config.view.dart_args) {
      spdlog::info(arg);
    }
  }
  spdlog::info("Bundle Path: .............. {}", config.view.bundle_path);
  spdlog::info("Window Type: .............. {}", config.view.window_type);
  if (config.view.backend.has_value() && !config.view.backend->empty()) {
    spdlog::info("Backend: .................. {}", config.view.backend.value());
  }
  spdlog::info("Output Index: ............. {}",
               config.view.wl_output_index.value_or(0));
  if (!config.view.output.empty()) {
    spdlog::info(
        "Output Match: ............. wl='{}' drm='{}' serial='{}' preload={} "
        "on_disconnect={}",
        config.view.output.wl_name.value_or("-"),
        config.view.output.drm_connector.value_or("-"),
        config.view.output.edid_serial.value_or("-"),
        config.view.output.preload,
        config.view.output.on_disconnect ==
                homescreen::OutputMatch::OnDisconnect::kTeardown
            ? "teardown"
            : "suspend");
  }
  spdlog::info("Size: ..................... {} x {}",
               config.view.width.value_or(kDefaultViewWidth),
               config.view.height.value_or(kDefaultViewHeight));
  spdlog::info("Pixel Ratio: .............. {0:.1f}",
               config.view.pixel_ratio.value_or(kDefaultPixelRatio));
  spdlog::info("Fullscreen: ............... {}",
               (config.view.fullscreen.value_or(false) ? "true" : "false"));
  spdlog::info("Accessibility Features: ... {}",
               config.view.accessibility_features.value_or(0));
  if (config.view.ivi_surface_id.has_value()) {
    spdlog::info("IVI Surface ID: ........... {}",
                 config.view.ivi_surface_id.value());
  }
#if ENABLE_AGL_SHELL_CLIENT
  spdlog::info("*******************");
  spdlog::info("* Activation Area *");
  spdlog::info("*******************");
  spdlog::info("x: ........................ {}", config.view.activation_area_x);
  spdlog::info("y: ........................ {}", config.view.activation_area_y);
  spdlog::info("width: .................... {}",
               config.view.activation_area_width);
  spdlog::info("height: ................... {}",
               config.view.activation_area_height);
#endif
}

std::vector<Configuration::Config> Configuration::ParseArgcArgv(
    const int argc,
    const char* const* argv) {
  Config config{};

  try {
    std::string accessibility_feature_flag_str;

    const std::unique_ptr<cxxopts::Options> allocated(
        new cxxopts::Options(kApplicationName, "Toyota Flutter Embedder"));

    allocated->set_width(80).set_tab_expansion().allow_unrecognised_options();

    // Core
    allocated->add_options()("h,help", "Print this help and exit")(
        "b,bundle",
        "Path to a bundle directory (repeatable; required unless --config "
        "is given)",
        cxxopts::value<std::vector<std::string>>(config.bundle_paths))(
        "config",
        "Path to a master config.toml defining one or more [[view]] entries "
        "(each with its own 'bundle'); overrides per-bundle config.toml",
        cxxopts::value<std::string>());

    // [global] (process-level)
    allocated->add_options("Global")(
        "app-id", "Application id (sets [global].app_id; used by all shells)",
        cxxopts::value<std::string>(config.app_id))(
        "xdg-shell-app-id", "Deprecated alias for --app-id",
        cxxopts::value<std::string>(config.app_id))(
        "t,cursor-theme", "Cursor theme name (e.g. DMZ-White)",
        cxxopts::value<std::string>(config.cursor_theme))(
        "c,disable-cursor", "Hide the pointer cursor",
        cxxopts::value<bool>()->implicit_value("true"))(
        "d,debug-backend", "Enable backend debug logging",
        cxxopts::value<bool>()->implicit_value("true"))(
        "wayland-event-mask",
        "Wayland input events to mask (e.g. pointer-axis,keyboard)",
        cxxopts::value<std::string>(config.wayland_event_mask));

    // [[view]] (geometry / Flutter)
    allocated->add_options("View")("w,width", "View width in px",
                                   cxxopts::value<uint32_t>())(
        "height", "View height in px", cxxopts::value<uint32_t>())(
        "o,output-index", "wl_output index to place the view on",
        cxxopts::value<uint32_t>())("p,pixel-ratio", "Device pixel ratio",
                                    cxxopts::value<double>())(
        "f,fullscreen", "Start fullscreen",
        cxxopts::value<bool>()->implicit_value("true"))(
        "a,accessibility-flags",
        "Accessibility feature bit mask (decimal / 0x.. / 0..)",
        cxxopts::value<std::string>(accessibility_feature_flag_str))(
        "engine-arg",
        "Engine / Dart VM switch -> command_line_argv (repeatable). "
        "Unrecognized options are also forwarded here.",
        cxxopts::value<std::vector<std::string>>())(
        "dart-arg",
        "Argument to the Dart entrypoint main(List<String>) -> "
        "dart_entrypoint_argv (repeatable)",
        cxxopts::value<std::vector<std::string>>());

    // [view.shell]
    allocated->add_options("Shell")(
        "shell",
        "Wayland compositor-protocol shell: auto|xdg|agl|ivi|simple "
        "(default auto)",
        cxxopts::value<std::string>())(
        "window-type", "AGL window role: BG|PANEL_*|NORMAL (AGL shell only)",
        cxxopts::value<std::string>(config.view.window_type))(
        "ivi-surface-id", "ivi-shell numeric surface id",
        cxxopts::value<uint32_t>());

    // [view.backend]
    allocated->add_options("Backend")(
        "backend",
        "Active backend: wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|"
        "software (default: env-aware -- a Wayland session selects "
        "wayland-egl, else drm-kms-egl)",
        cxxopts::value<std::string>());

    // [view.backend.drm] (also settable via HOMESCREEN_DRM_* env)
    allocated->add_options("DRM")(
        "drm-list-modes",
        "List the active backend's scanout modes and exit (optional device "
        "path; defaults to --drm-device or the backend default)",
        cxxopts::value<std::string>()->implicit_value(""))(
        "drm-device", "DRM device path (e.g. /dev/dri/card0)",
        cxxopts::value<std::string>())(
        "drm-connector",
        "DRM connector to drive (e.g. eDP-1, HDMI-A-1); default rank-picks",
        cxxopts::value<std::string>())(
        "drm-mode",
        "DRM mode <WxH@R> (e.g. 1920x1080@120); default = preferred mode",
        cxxopts::value<std::string>())(
        "drm-rotation",
        "DRM scanout rotation in degrees: 0|90|180|270 (default 0)",
        cxxopts::value<int>())("drm-compositor",
                               "DRM compositor strategy: auto|planes|gl",
                               cxxopts::value<std::string>())(
        "drm-modeset", "DRM modeset API: auto|legacy|atomic",
        cxxopts::value<std::string>())(
        "drm-allow-nonblock-modeset",
        "Allow NONBLOCK | ALLOW_MODESET atomic commits: auto|yes|no",
        cxxopts::value<std::string>())(
        "drm-primary-format",
        "Primary plane format: auto|xrgb8888|xbgr8888|argb8888|abgr8888|rgb565",
        cxxopts::value<std::string>())("drm-overlay-planes",
                                       "Use overlay planes: auto|yes|no",
                                       cxxopts::value<std::string>())(
        "drm-explicit-sync",
        "Use IN_FENCE_FD / OUT_FENCE_PTR on commits: auto|yes|no",
        cxxopts::value<std::string>())(
        "drm-async-flip",
        "Use DRM_MODE_PAGE_FLIP_ASYNC on flip-only commits: auto|yes|no",
        cxxopts::value<std::string>())(
        "drm-stage-cursor",
        "Stage the HW cursor into the compositor commit: auto|yes|no",
        cxxopts::value<std::string>())(
        "drm-no-seat",
        "Bypass libseat: open /dev/dri + input directly and self-acquire "
        "DRM master, skipping the foreground-VT guard (headless/SSH/kiosk; "
        "you must ensure nothing else holds the display)",
        cxxopts::value<bool>()->implicit_value("true"))(
        "input-transform",
        "Per-device pointer transform (repeatable): "
        "\"<device-name-substring>=<0|90|180|270>[,flip-x][,flip-y]\"",
        cxxopts::value<std::vector<std::string>>());

    const auto result = allocated->parse(argc, argv);

    if (result.count("help")) {
      spdlog::info("{}", allocated->help({"", "Global", "View", "Shell",
                                          "Backend", "DRM"}));
      exit(EXIT_SUCCESS);
    }

    if (result.count("config")) {
      config.config_file = result["config"].as<std::string>();
    }

    // Bundle-directory validation happens after parse_config(), since with
    // --config the bundle paths come from the file's [[view]] entries rather
    // than from -b.

    if (result.count("accessibility-flags")) {
      if (accessibility_feature_flag_str.empty()) {
        spdlog::critical(
            "-a option (Accessibility Features) requires an "
            "argument (e.g. -a 31)");
        exit(EXIT_FAILURE);
      }
      try {
        // The following styles are acceptable:
        // 1. decimal: --a=31
        // 2. hex: --a=0x3
        // 3. octet: --a=03
        config.view.accessibility_features = static_cast<int32_t>(
            std::stol(accessibility_feature_flag_str, nullptr, 0));
      } catch (const std::invalid_argument& /* e */) {
        spdlog::critical(
            "-a option (Accessibility Features) requires an integer value");
        exit(EXIT_FAILURE);
      } catch (const std::out_of_range& /* e */) {
        spdlog::critical(
            "The specified value to -a option, {} is out of range.",
            accessibility_feature_flag_str);
        exit(EXIT_FAILURE);
      }
      config.view.accessibility_features = mask_accessibility_features(
          config.view.accessibility_features.value());
    }

    if (result.count("wayland-event-mask")) {
      if (config.wayland_event_mask.empty()) {
        spdlog::critical(
            "--wayland-event-mask option requires an argument "
            "(e.g. --wayland-event-mask pointer-axis,keyboard)");
        exit(EXIT_FAILURE);
      }
    }

    if ((result.count("app-id") || result.count("xdg-shell-app-id")) &&
        config.app_id.empty()) {
      spdlog::critical("--app-id requires an argument (e.g. --app-id gallery)");
      exit(EXIT_FAILURE);
    }

    if (result.count("cursor-theme")) {
      if (config.cursor_theme.empty()) {
        spdlog::critical("-t option requires an argument (e.g. -t DMZ-White)");
        exit(EXIT_FAILURE);
      }
    }

    if (result.count("window-type")) {
      if (config.view.window_type.empty()) {
        spdlog::critical(
            "--window-type requires an argument (e.g. --window-type BG)");
        exit(EXIT_FAILURE);
      }
    }

    if (result.count("engine-arg")) {
      for (const auto& a :
           result["engine-arg"].as<std::vector<std::string>>()) {
        config.view.engine_args.emplace_back(a);
      }
    }
    if (result.count("dart-arg")) {
      for (const auto& a : result["dart-arg"].as<std::vector<std::string>>()) {
        config.view.dart_args.emplace_back(a);
      }
    }

    if (result.count("pixel-ratio")) {
      if (config.view.pixel_ratio == 0) {
        spdlog::critical("-p option (Pixel Ratio) requires a non-zero value");
        exit(EXIT_FAILURE);
      }
      config.view.pixel_ratio = result["pixel-ratio"].as<double>();
    }

    if (result.count("disable-cursor")) {
      config.disable_cursor = result["disable-cursor"].as<bool>();
    }
    if (result.count("debug-backend")) {
      config.debug_backend = result["debug-backend"].as<bool>();
    }
    if (result.count("fullscreen")) {
      config.view.fullscreen = result["fullscreen"].as<bool>();
    }
    if (result.count("width")) {
      config.view.width = result["width"].as<uint32_t>();
    }
    if (result.count("height")) {
      config.view.height = result["height"].as<uint32_t>();
    }
    if (result.count("output-index")) {
      config.view.wl_output_index = result["output-index"].as<uint32_t>();
    }
    if (result.count("ivi-surface-id")) {
      config.view.ivi_surface_id = result["ivi-surface-id"].as<uint32_t>();
    }
    if (result.count("backend")) {
      config.view.backend = result["backend"].as<std::string>();
    }
    if (result.count("shell")) {
      config.view.shell = result["shell"].as<std::string>();
    }

    // DRM knobs. CLI wins; env (HOMESCREEN_DRM_*) fills only when the
    // matching CLI flag was absent. Empty string is treated as unset.
    auto pick_string = [&](const char* cli_name, const char* env_name,
                           std::optional<std::string>& out) {
      if (result.count(cli_name)) {
        const auto v = result[cli_name].as<std::string>();
        if (!v.empty()) {
          out = v;
          return;
        }
      }
      if (const char* e = std::getenv(env_name); e && *e) {
        out = std::string(e);
      }
    };
    pick_string("drm-device", "HOMESCREEN_DRM_DEVICE", config.view.drm_device);
    pick_string("drm-connector", "HOMESCREEN_DRM_CONNECTOR",
                config.view.drm_connector);
    pick_string("drm-mode", "HOMESCREEN_DRM_MODE", config.view.drm_mode);
    pick_string("drm-compositor", "HOMESCREEN_DRM_COMPOSITOR",
                config.view.drm_compositor);
    pick_string("drm-modeset", "HOMESCREEN_DRM_MODESET",
                config.view.drm_modeset);
    pick_string("drm-allow-nonblock-modeset",
                "HOMESCREEN_DRM_ALLOW_NONBLOCK_MODESET",
                config.view.drm_allow_nonblock_modeset);
    pick_string("drm-primary-format", "HOMESCREEN_DRM_PRIMARY_FORMAT",
                config.view.drm_primary_format);
    pick_string("drm-overlay-planes", "HOMESCREEN_DRM_OVERLAY_PLANES",
                config.view.drm_overlay_planes);
    pick_string("drm-explicit-sync", "HOMESCREEN_DRM_EXPLICIT_SYNC",
                config.view.drm_explicit_sync);
    pick_string("drm-async-flip", "HOMESCREEN_DRM_ASYNC_FLIP",
                config.view.drm_async_flip);
    pick_string("drm-stage-cursor", "HOMESCREEN_DRM_STAGE_CURSOR",
                config.view.drm_stage_cursor);

    // --drm-no-seat (bool flag) / HOMESCREEN_DRM_NO_SEAT (env). CLI wins;
    // env fills only when the flag is absent. Env is truthy on
    // 1 / true / yes (case-sensitive); anything else (incl. empty) = unset.
    if (result.count("drm-no-seat")) {
      config.view.drm_no_seat = result["drm-no-seat"].as<bool>();
    } else if (const char* e = std::getenv("HOMESCREEN_DRM_NO_SEAT"); e && *e) {
      const std::string_view v(e);
      config.view.drm_no_seat = (v == "1" || v == "true" || v == "yes");
    }

    // drm-rotation is an int (0|90|180|270); CLI wins, then env. Reject any
    // other value so a typo fails loud instead of silently scanning unrotated.
    {
      std::optional<int> rot;
      if (result.count("drm-rotation")) {
        rot = result["drm-rotation"].as<int>();
      } else if (const char* e = std::getenv("HOMESCREEN_DRM_ROTATION");
                 e && *e) {
        rot = static_cast<int>(std::strtol(e, nullptr, 10));
      }
      if (rot.has_value()) {
        if (*rot != 0 && *rot != 90 && *rot != 180 && *rot != 270) {
          spdlog::critical("--drm-rotation must be 0, 90, 180, or 270 (got {})",
                           *rot);
          exit(EXIT_FAILURE);
        }
        config.view.drm_rotation = *rot;
      }
    }

    // Per-device pointer transforms (repeatable). Validated downstream in
    // DrmSeat::SetInputTransforms, which warns + skips malformed specs.
    if (result.count("input-transform")) {
      config.view.drm_input_transforms =
          result["input-transform"].as<std::vector<std::string>>();
    }

    // Unrecognized CLI options are forwarded as engine / Dart VM switches
    // (command_line_argv).
    config.view.engine_args.reserve(result.unmatched().size());
    for (const auto& option : result.unmatched()) {
      config.view.engine_args.emplace_back(option.c_str());
    }

  } catch (const cxxopts::exceptions::exception& e) {
    spdlog::critical("Failed to Convert Command Line: {}", e.what());
    exit(EXIT_FAILURE);
  }

  auto configs = parse_config(config);

  // Validate the resolved view list: at least one view, and every bundle must
  // be an existing directory (paths may originate from -b or from a --config
  // file's [[view]] entries).
  if (configs.empty()) {
    spdlog::critical(
        "No views configured: provide -b <bundle> or --config <file>");
    exit(EXIT_FAILURE);
  }
  for (const auto& c : configs) {
    if (!std::filesystem::is_directory(c.view.bundle_path)) {
      spdlog::critical("Bundle path is not a directory: {}",
                       c.view.bundle_path);
      exit(EXIT_FAILURE);
    }
  }

  if (!config.view.fullscreen) {
    if (config.view.width == 0) {
      spdlog::critical("-w option (Width) requires an argument (e.g. -w 720)");
      exit(EXIT_FAILURE);
    }
    if (config.view.height == 0) {
      spdlog::critical(
          "-h option (Height) requires an argument (e.g. -w 1280)");
      exit(EXIT_FAILURE);
    }
  }

  for (auto const& c : configs) {
    PrintConfig(c);
  }

  return configs;
}

int32_t Configuration::mask_accessibility_features(
    int32_t accessibility_features) {
  accessibility_features &= 0b1111111;
  return accessibility_features;
}
