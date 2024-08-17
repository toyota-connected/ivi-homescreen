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

#include <filesystem>

#include "config/common.h"
#include "cxxopts/include/cxxopts.hpp"
#include "utils.h"

void Configuration::get_parameters(toml::table* tbl, Config& instance) {
  if (tbl->at_path("global.app_id").is_string()) {
    instance.app_id = tbl->at_path("global.app_id").as_string()->value_or("");
  }
  if (tbl->at_path("global.cursor_theme").is_string()) {
    instance.cursor_theme =
        tbl->at_path("global.cursor_theme").as_string()->value_or("");
  }
  if (tbl->at_path("global.disable_cursor").is_boolean()) {
    instance.disable_cursor =
        tbl->at_path("global.disable_cursor").value<bool>().value();
  }
  if (tbl->at_path("global.wayland_event_mask").is_string()) {
    instance.wayland_event_mask =
        tbl->at_path("global.wayland_event_mask").as_string()->value_or("");
  }
  if (tbl->at_path("global.debug_backend").is_boolean()) {
    instance.debug_backend =
        tbl->at_path("global.debug_backend").value<bool>().value();
  }

  if (tbl->at_path("view.window_type").is_string()) {
    instance.view.window_type =
        tbl->at_path("view.window_type").as_string()->value_or("");
  }
  if (tbl->at_path("view.output_index").is_integer()) {
    instance.view.wl_output_index =
        tbl->at_path("view.output_index").value<uint32_t>().value();
  }
  if (tbl->at_path("view.width").is_integer()) {
    instance.view.width = tbl->at_path("view.width").value<uint32_t>().value();
  }
  if (tbl->at_path("view.height").is_integer()) {
    instance.view.height =
        tbl->at_path("view.height").value<uint32_t>().value();
  }
  if (tbl->at_path("view.pixel_ratio").is_floating_point()) {
    instance.view.pixel_ratio =
        tbl->at_path("view.pixel_ratio").value<double>().value();
  }
  if (tbl->at_path("view.ivi_surface_id").is_integer()) {
    instance.view.ivi_surface_id =
        tbl->at_path("view.ivi_surface_id").value<uint32_t>().value();
  }
  if (tbl->at_path("view.accessibility_features").is_integer()) {
    instance.view.accessibility_features =
        tbl->at_path("view.accessibility_features").value<int32_t>().value();
  }
  if (tbl->at_path("view.vm_args").is_array()) {
    const auto vm_args = tbl->at_path("view.vm_args").as_array();
    for (auto& arg : *vm_args) {
      instance.view.vm_args.emplace_back(arg.as_string()->value_or(""));
    }
  }
  if (tbl->at_path("view.fullscreen").is_boolean()) {
    instance.view.fullscreen =
        tbl->at_path("view.fullscreen").value<bool>().value();
  }
  if (tbl->at_path("view.fps_output_console").is_integer()) {
    instance.view.fps_output_console =
        tbl->at_path("view.fps_output_console").value<uint32_t>().value();
  }
  if (tbl->at_path("view.fps_output_overlay").is_integer()) {
    instance.view.fps_output_overlay =
        tbl->at_path("view.fps_output_overlay").value<uint32_t>().value();
  }
  if (tbl->at_path("view.fps_output_frequency").is_integer()) {
    instance.view.fps_output_frequency =
        tbl->at_path("view.fps_output_frequency").value<uint32_t>().value();
  }

  if (tbl->at_path("window_activation_area.x").is_integer()) {
    instance.view.activation_area_x =
        tbl->at_path("window_activation_area.x").value<uint32_t>().value();
  }
  if (tbl->at_path("window_activation_area.y").is_integer()) {
    instance.view.activation_area_y =
        tbl->at_path("window_activation_area.y").value<uint32_t>().value();
  }
  if (tbl->at_path("window_activation_area.width").is_integer()) {
    instance.view.activation_area_width =
        tbl->at_path("window_activation_area.width").value<uint32_t>().value();
  }
  if (tbl->at_path("window_activation_area.height").is_integer()) {
    instance.view.activation_area_height =
        tbl->at_path("window_activation_area.height").value<uint32_t>().value();
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
  get_parameters(&tbl, instance);
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
  if (!cli.view.vm_args.empty()) {
    for (auto const& arg : cli.view.vm_args) {
      instance.view.vm_args.emplace_back(arg);
    }
  }
  if (!cli.view.window_type.empty()) {
    instance.view.window_type = cli.view.window_type;
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
}

std::vector<Configuration::Config> Configuration::parse_config(
    const Config& cli_config) {
  const auto view_count = cli_config.bundle_paths.size();

  std::vector<Config> res;
  res.reserve(view_count);

  for (const auto& bundle_path : cli_config.bundle_paths) {
    Config cfg{};

    const auto config_toml_path =
        std::string(bundle_path) + "/" + kViewConfigToml;
    get_toml_config(config_toml_path.c_str(), cfg);
    get_cli_override(bundle_path, cfg, cli_config);

    if (cfg.view.window_type.empty()) {
      cfg.view.window_type = "NORMAL";
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
      cfg.app_id = kApplicationName;
    }

    res.emplace_back(cfg);
  }
  assert(res.capacity() == view_count);

  return std::move(res);
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
  spdlog::info("Debug Backend: ........... {}",
               (config.debug_backend.value_or(false) ? "true" : "false"));
  spdlog::info("********");
  spdlog::info("* View *");
  spdlog::info("********");
  if (!config.view.vm_args.empty()) {
    spdlog::info("VM Args:");
    for (auto const& arg : config.view.vm_args) {
      spdlog::info(arg);
    }
  }
  spdlog::info("Bundle Path: .............. {}", config.view.bundle_path);
  spdlog::info("Window Type: .............. {}", config.view.window_type);
  spdlog::info("Output Index: ............. {}",
               config.view.wl_output_index.value_or(0));
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

    allocated->set_width(80)
        .set_tab_expansion()
        .allow_unrecognised_options()
        .add_options()("help", "Print help")(
            "b,bundle", "Path to a bundle directory (required)",
            cxxopts::value<std::vector<std::string>>(config.bundle_paths))(
            "a,accessibility-flags", "Accessibility feature flag(s)",
            cxxopts::value<std::string>(accessibility_feature_flag_str))(
            "c,disable-cursor", "Disable cursor",
            cxxopts::value<bool>())(
            "d,debug-backend", "Debug backend",
            cxxopts::value<bool>())(
            "f,fullscreen", "Full screen",
            cxxopts::value<bool>())(
            "w,width", "Width", cxxopts::value<uint32_t>())(
            "h,height", "Height",
            cxxopts::value<uint32_t>())(
            "p,pixel-ratio", "Pixel Ratio",
            cxxopts::value<double>())(
            "t,cursor-theme", "Cursor Theme Name",
            cxxopts::value<std::string>(config.cursor_theme))(
            "window-type", "AGL window type (only applies to AGL-compositor)",
            cxxopts::value<std::string>(config.view.window_type))(
            "o,output-index", "Wayland output index",
            cxxopts::value<uint32_t>())(
            "xdg-shell-app-id", "XDG shell app id",
            cxxopts::value<std::string>(config.app_id))(
            "event-mask", "Wayland Events to mask",
            cxxopts::value<std::string>(config.wayland_event_mask))(
            "ivi-surface-id", "IVI Surface ID",
            cxxopts::value<uint32_t>());

    const auto result = allocated->parse(argc, argv);

    if (result.count("help")) {
      spdlog::info("{}", allocated->help({"", "Group"}));
      exit(EXIT_SUCCESS);
    }

    if (config.bundle_paths.empty()) {
      spdlog::critical(
          "-b (Bundle Path) option requires at least one directory path "
          "argument (e.g. -b /usr/share/gallery)");
    }
    for (const auto& path : config.bundle_paths) {
      if (!std::filesystem::is_directory(path)) {
        spdlog::critical("Bundle path is not a directory: {}", path);
        exit(EXIT_FAILURE);
      }
    }

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

    if (result.count("event-mask")) {
      if (config.wayland_event_mask.empty()) {
        spdlog::critical(
            "--wayland-event-mask option requires an argument "
            "(e.g. --wayland-event-mask pointer-axis,keyboard)");
        exit(EXIT_SUCCESS);
      }
    }

    if (result.count("xdg-shell-app-id")) {
      if (config.app_id.empty()) {
        spdlog::critical(
            "-xdg-shell-app-id option requires an argument "
            "(e.g. -xdg-shell-app-id gallery)");
        exit(EXIT_FAILURE);
      }
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
            "-window-type option requires an argument (e.g. "
            "-window-type BG)");
        exit(EXIT_FAILURE);
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

    config.view.vm_args.reserve(result.unmatched().size());
    for (const auto& option : result.unmatched()) {
      config.view.vm_args.emplace_back(option.c_str());
    }

  } catch (const cxxopts::exceptions::exception& e) {
    spdlog::critical("Failed to Convert Command Line: {}", e.what());
    exit(EXIT_FAILURE);
  }

  auto configs = parse_config(config);

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

  return std::move(configs);
}

int32_t Configuration::mask_accessibility_features(
    int32_t accessibility_features) {
  accessibility_features &= 0b1111111;
  return accessibility_features;
}
