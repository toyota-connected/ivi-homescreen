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
#include <fstream>
#include <sstream>

#include "config/common.h"

#include <rapidjson/document.h>
#include "utils.h"

rapidjson::SizeType Configuration::getViewCount(rapidjson::Document& doc) {
  if (!doc.HasMember(kViewKey)) {
    spdlog::critical("JSON Configuration requires a \"view\" object");
    exit(EXIT_FAILURE);
  }

  if (doc[kViewKey].IsArray()) {
    return doc[kViewKey].GetArray().Capacity();
  }

  return 1;
}

void Configuration::getViewParameters(
    const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
    Config& instance) {
  if (obj.HasMember(kBundlePathKey) && obj[kBundlePathKey].IsString()) {
    instance.view.bundle_path = obj[kBundlePathKey].GetString();
  }
  if (obj.HasMember(kWindowTypeKey) && obj[kWindowTypeKey].IsString()) {
    instance.view.window_type = obj[kWindowTypeKey].GetString();
  }
  if (obj.HasMember(kOutputIndex) && obj[kOutputIndex].IsUint()) {
    instance.view.wl_output_index =
        static_cast<uint32_t>(obj[kOutputIndex].GetUint());
  }
  if (obj.HasMember(kWidthKey) && obj[kWidthKey].IsUint()) {
    instance.view.width = obj[kWidthKey].GetUint();
  }
  if (obj.HasMember(kHeightKey) && obj[kHeightKey].IsUint()) {
    instance.view.height = obj[kHeightKey].GetUint();
  }
  if (obj.HasMember(kPixelRatioKey)) {
    if (obj[kPixelRatioKey].IsDouble() || obj[kPixelRatioKey].IsInt()) {
      instance.view.pixel_ratio = obj[kPixelRatioKey].GetDouble();
    }
  }
  if (obj.HasMember(kIviSurfaceIdKey) && obj[kIviSurfaceIdKey].IsUint()) {
    instance.view.ivi_surface_id = obj[kIviSurfaceIdKey].GetUint();
  }
  if (obj.HasMember(kAccessibilityFeaturesKey) &&
      obj[kAccessibilityFeaturesKey].IsInt()) {
    instance.view.accessibility_features =
        MaskAccessibilityFeatures(obj[kAccessibilityFeaturesKey].GetInt());
  }
  if (obj.HasMember(kVmArgsKey) && obj[kVmArgsKey].IsArray()) {
    const auto args = obj[kVmArgsKey].GetArray();
    for (auto const& arg : args) {
      instance.view.vm_args.emplace_back(arg.GetString());
    }
  }
  if (obj.HasMember(kFullscreenKey) && obj[kFullscreenKey].IsBool()) {
    instance.view.fullscreen = obj[kFullscreenKey].GetBool();
  }
  if (obj.HasMember(kDebugBackendKey) && obj[kDebugBackendKey].IsBool()) {
    instance.debug_backend = obj[kDebugBackendKey].GetBool();
  }
  if (obj.HasMember(kFpsOutputConsole) && obj[kFpsOutputConsole].IsUint()) {
    instance.view.fps_output_console = obj[kFpsOutputConsole].GetUint();
  }
  if (obj.HasMember(kFpsOutputOverlay) && obj[kFpsOutputOverlay].IsUint()) {
    instance.view.fps_output_overlay = obj[kFpsOutputOverlay].GetUint();
  }
  if (obj.HasMember(kFpsOutputFrequency) && obj[kFpsOutputFrequency].IsUint()) {
    instance.view.fps_output_frequency = obj[kFpsOutputFrequency].GetUint();
  }

  if (obj.HasMember(kWindowActivationAreaKey)) {
    const auto val = obj[kWindowActivationAreaKey].GetObject();

    instance.view.activation_area_x = static_cast<uint32_t>(val["x"].GetInt());
    instance.view.activation_area_y = static_cast<uint32_t>(val["y"].GetInt());
    instance.view.activation_area_width =
        static_cast<uint32_t>(val["width"].GetInt());
    instance.view.activation_area_height =
        static_cast<uint32_t>(val["height"].GetInt());

    SPDLOG_DEBUG("activation area x {}", instance.view.activation_area_x);
    SPDLOG_DEBUG("activation area y {}", instance.view.activation_area_y);
    SPDLOG_DEBUG("activation area width {}",
                 instance.view.activation_area_width);
    SPDLOG_DEBUG("activation area height {}",
                 instance.view.activation_area_height);
  }
}

void Configuration::getGlobalParameters(
    const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
    Config& instance) {
  if (obj.HasMember(kAppIdKey) && obj[kAppIdKey].IsString()) {
    instance.app_id = obj[kAppIdKey].GetString();
  }
  if (obj.HasMember(kCursorThemeKey) && obj[kCursorThemeKey].IsString()) {
    instance.cursor_theme = obj[kCursorThemeKey].GetString();
  }
  if (obj.HasMember(kDisableCursorKey) && obj[kDisableCursorKey].IsBool()) {
    instance.disable_cursor = obj[kDisableCursorKey].GetBool();
  }
  if (obj.HasMember(kWaylandEventMaskKey) &&
      obj[kWaylandEventMaskKey].IsString()) {
    instance.wayland_event_mask = obj[kWaylandEventMaskKey].GetString();
  }
  if (obj.HasMember(kDebugBackendKey) && obj[kDebugBackendKey].IsBool()) {
    instance.debug_backend = obj[kDebugBackendKey].GetBool();
  }

  Configuration::getViewParameters(obj, instance);
}

void Configuration::getView(rapidjson::Document& doc,
                            int index,
                            Config& instance) {
  if (doc[kViewKey].IsArray()) {
    const auto arr = doc[kViewKey].GetArray();
    if (index > arr.Capacity())
      assert(false);

    getViewParameters(arr[static_cast<rapidjson::SizeType>(index)].GetObject(),
                      instance);
    getGlobalParameters(doc.GetObject(), instance);
  } else {
    getViewParameters(doc[kViewKey].GetObject(), instance);
    getGlobalParameters(doc.GetObject(), instance);
  }
}

void Configuration::getCliOverrides(Config& instance, const Config& cli) {
  if (!cli.app_id.empty()) {
    instance.app_id = cli.app_id;
  }
  if (!cli.json_configuration_path.empty()) {
    instance.json_configuration_path = cli.json_configuration_path;
  }
  if (!cli.cursor_theme.empty()) {
    instance.cursor_theme = cli.cursor_theme;
  }
  if (cli.disable_cursor_set && cli.disable_cursor != instance.disable_cursor) {
    instance.disable_cursor = cli.disable_cursor;
  }
  if (!cli.wayland_event_mask.empty()) {
    instance.wayland_event_mask = cli.wayland_event_mask;
  }
  if (cli.debug_backend_set && cli.debug_backend != instance.debug_backend) {
    instance.debug_backend = cli.debug_backend;
  }
  if (!cli.view.vm_args.empty()) {
    for (auto const& arg : cli.view.vm_args) {
      instance.view.vm_args.emplace_back(arg);
    }
  }
  if (!cli.view.bundle_path.empty()) {
    instance.view.bundle_path = cli.view.bundle_path;
  }
  if (!cli.view.window_type.empty()) {
    instance.view.window_type = cli.view.window_type;
  }
  if (cli.view.wl_output_index > 0) {
    instance.view.wl_output_index = cli.view.wl_output_index;
  }
  if (cli.view.accessibility_features > 0) {
    instance.view.accessibility_features =
        MaskAccessibilityFeatures(cli.view.accessibility_features);
  }
  if (cli.view.width > 0) {
    instance.view.width = cli.view.width;
  }
  if (cli.view.height > 0) {
    instance.view.height = cli.view.height;
  }
  if (cli.view.pixel_ratio > 0) {
    instance.view.pixel_ratio = cli.view.pixel_ratio;
  }
  if (cli.view.ivi_surface_id > 0) {
    instance.view.ivi_surface_id = cli.view.ivi_surface_id;
  }
  if (cli.view.fullscreen_set &&
      cli.view.fullscreen != instance.view.fullscreen) {
    instance.view.fullscreen = cli.view.fullscreen;
  }
}

std::vector<Configuration::Config> Configuration::ParseConfig(
    const Config& config) {
  rapidjson::Document doc;
  rapidjson::SizeType view_count = 1;
  if (!config.json_configuration_path.empty()) {
    doc = getJsonDocument(config.json_configuration_path);
    if (!doc.IsObject()) {
      spdlog::critical("Invalid JSON Configuration file");
      exit(EXIT_FAILURE);
    }

    view_count = getViewCount(doc);
  }

  SPDLOG_DEBUG("View Count: {}", view_count);
  std::vector<Config> res;
  res.reserve(view_count);
  for (int i = 0; i < view_count; i++) {
    Config cfg{};
    if (doc.IsObject()) {
      getView(doc, i, cfg);
    }
    getCliOverrides(cfg, config);

    if (cfg.view.window_type.empty())
      cfg.view.window_type = "NORMAL";

    if (cfg.view.bundle_path.empty()) {
      spdlog::critical("A bundle path must be specified");
      exit(EXIT_FAILURE);
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

  return res;
}

rapidjson::Document Configuration::getJsonDocument(
    const std::string& filename) {
  std::ifstream json_file(filename);
  if (!json_file.is_open()) {
    spdlog::critical("Unable to open file {}", filename);
    exit(EXIT_FAILURE);
  }

  std::stringstream contents;
  contents << json_file.rdbuf();

  rapidjson::Document doc;
  doc.Parse(contents.str().c_str());
  return doc;
}

void Configuration::PrintConfig(const Config& config) {
  spdlog::info("{} @ {}", kGitBranch, kGitCommitHash);

  spdlog::info("**********");
  spdlog::info("* Global *");
  spdlog::info("**********");
  if (!config.app_id.empty()) {
    spdlog::info("Application Id: .......... {}", config.app_id);
  }
  if (!config.json_configuration_path.empty()) {
    spdlog::info("JSON Configuration: ...... {}",
                 config.json_configuration_path);
  }
  if (!config.cursor_theme.empty()) {
    spdlog::info("Cursor Theme: ............ {}", config.cursor_theme);
  }
  spdlog::info("Disable Cursor: .......... {}",
               (config.disable_cursor ? "true" : "false"));
  if (!config.wayland_event_mask.empty()) {
    spdlog::info("Wayland Event Mask: ...... {}", config.wayland_event_mask);
  }
  spdlog::info("Debug Backend: ........... {}",
               (config.debug_backend ? "true" : "false"));
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
  spdlog::info("Output Index: ............. {}", config.view.wl_output_index);
  spdlog::info("Size: ..................... {} x {}", config.view.width,
               config.view.height);
  if (config.view.pixel_ratio != kDefaultPixelRatio) {
    spdlog::info("Pixel Ratio: .............. {}", config.view.pixel_ratio);
  }
  if (config.view.ivi_surface_id > 0) {
    spdlog::info("IVI Surface ID: ........... {}", config.view.ivi_surface_id);
  }
  spdlog::info("Fullscreen: ............... {}",
               (config.view.fullscreen ? "true" : "false"));
  spdlog::info("Accessibility Features: ... {}",
               config.view.accessibility_features);
}

Configuration::Config Configuration::ParseArgcArgv(const int argc,
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
            cxxopts::value<std::string>(config.view.bundle_path))(
            "j,json-config", "Path to a json configuration file",
            cxxopts::value<std::string>(config.json_configuration_path))(
            "a,accessibility-flags", "Accessibility feature flag(s)",
            cxxopts::value<std::string>(accessibility_feature_flag_str))(
            "c,disable-cursor", "Disable cursor",
            cxxopts::value<bool>(config.disable_cursor))(
            "d,debug-backend", "Debug backend",
            cxxopts::value<bool>(config.debug_backend))(
            "f,fullscreen", "Full screen",
            cxxopts::value<bool>(config.view.fullscreen))(
            "w,width", "Width", cxxopts::value<uint32_t>(config.view.width))(
            "h,height", "Height", cxxopts::value<uint32_t>(config.view.height))(
            "p,pixel-ratio", "Pixel Ratio",
            cxxopts::value<double>(config.view.pixel_ratio))(
            "t,cursor-theme", "Cursor Theme Name",
            cxxopts::value<std::string>(config.cursor_theme))(
            "window-type", "AGL window type (only applies to AGL-compositor)",
            cxxopts::value<std::string>(config.view.window_type))(
            "o,output-index", "Wayland output index",
            cxxopts::value<uint32_t>(config.view.wl_output_index))(
            "xdg-shell-app-id", "XDG shell app id",
            cxxopts::value<std::string>(config.app_id))(
            "event-mask", "Wayland Events to mask",
            cxxopts::value<std::string>(config.wayland_event_mask))(
            "ivi-surface-id", "IVI Surface ID",
            cxxopts::value<uint32_t>(config.view.ivi_surface_id));

    const auto result = allocated->parse(argc, argv);

    if (result.count("help")) {
      spdlog::info("{}", allocated->help({"", "Group"}));
      exit(EXIT_SUCCESS);
    }

    if (config.view.bundle_path.empty() ||
        !std::filesystem::is_directory(config.view.bundle_path)) {
      spdlog::critical(
          "-b (Bundle Path) option requires a directory path "
          "argument (e.g. "
          "-b /usr/share/gallery)");
      exit(EXIT_FAILURE);
    }

    if (!config.json_configuration_path.empty() &&
        !std::filesystem::exists(config.json_configuration_path)) {
      spdlog::critical(
          "-j option requires an argument (e.g. "
          "-j /tmp/cfg-dbg.json)");
      exit(EXIT_FAILURE);
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
      config.view.accessibility_features =
          MaskAccessibilityFeatures(config.view.accessibility_features);
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
    }

    if (!config.view.fullscreen) {
      if (config.view.width == 0) {
        spdlog::critical(
            "-w option (Width) requires an argument (e.g. -w 720)");
        exit(EXIT_FAILURE);
      }
      if (config.view.height == 0) {
        spdlog::critical(
            "-h option (Height) requires an argument (e.g. -w 1280)");
        exit(EXIT_FAILURE);
      }
    }

    if (config.disable_cursor) {
      config.disable_cursor_set = true;
    }
    if (config.debug_backend) {
      config.debug_backend_set = true;
    }
    if (config.view.fullscreen) {
      config.view.fullscreen_set = true;
    }

    config.view.vm_args.reserve(result.unmatched().size());
    for (const auto& option : result.unmatched()) {
      config.view.vm_args.emplace_back(option.c_str());
    }

  } catch (const cxxopts::exceptions::exception& e) {
    spdlog::critical("Failed to Convert Command Line: {}", e.what());
    exit(EXIT_FAILURE);
  }

  return std::move(config);
}

int32_t Configuration::MaskAccessibilityFeatures(
    int32_t accessibility_features) {
  accessibility_features &= 0b1111111;
  return accessibility_features;
}
