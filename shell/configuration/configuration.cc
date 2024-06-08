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

#include "config.h"

#include <rapidjson/document.h>
#include "utils.h"

rapidjson::SizeType Configuration::getViewCount(rapidjson::Document& doc) {
  if (!doc.HasMember(kViewKey)) {
    spdlog::critical("JSON Configuration requires a \"view\" object");
    exit(EXIT_FAILURE);
  }

  if (doc[kViewKey].IsArray()) {
    return doc[kViewKey].GetArray().Capacity();
  } else {
    return 1;
  }
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

std::vector<struct Configuration::Config> Configuration::ParseConfig(
    Configuration::Config& config) {
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
  std::vector<struct Config> res;
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

Configuration::Config Configuration::ConfigFromArgcArgv(
    int argc,
    const char* const* argv) {
  struct Configuration::Config config {};
  config.view.vm_args.reserve(static_cast<unsigned long>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    config.view.vm_args.emplace_back(argv[i]);
  }

  auto cl = fml::CommandLineFromArgcArgv(argc, argv);

  if (EXIT_SUCCESS != Configuration::ConvertCommandlineToConfig(cl, config)) {
    spdlog::critical("Failed to Convert Command Line");
    exit(EXIT_FAILURE);
  }

  return config;
}

void PrintUsage() {
  fprintf(
      stdout,
      "Usage: %s [options] [flutter VM arguments]\n"
      "Options:\n"
      "\t--b=<dir path>                Path to a bundle directory (required)\n"
      "\t--j=<file path>               Path to a json configuration file\n"
      "\t--a=<integer>                 Accessibility feature flag(s)\n"
      "\t--c                           Disable cursor\n"
      "\t--d                           Debug backend\n"
      "\t--f                           Full screen\n"
      "\t--w=<integer>                 Output width in pixels\n"
      "\t--h=<integer>                 Output height in pixels\n"
      "\t--p=<decimal>                 Pixel ratio\n"
      "\t--t=<string>                  Cursor theme name\n"
      "\t--window-type=<string>        AGL window type (only applies to "
      "AGL-compositor)\n"
      "\t--output-index=<integer>      Wayland output index\n"
      "\t--xdg-shell-app-id=<string>   XDG shell app id\n"
      "\t--wayland-event-mask=<string> Wayland events to mask\n"
      "\t--i=<ivi surface id>          IVI Surface ID\n",
      kApplicationName);
}

int Configuration::ConvertCommandlineToConfig(const fml::CommandLine& cl,
                                              Configuration::Config& config) {
  if (!cl.options().empty()) {
    if (cl.HasOption("?") || cl.HasOption("help")) {
      PrintUsage();
      exit(EXIT_SUCCESS);
    }
    if (cl.HasOption("j")) {
      cl.GetOptionValue("j", &config.json_configuration_path);
      if (config.json_configuration_path.empty()) {
        spdlog::critical(
            "--j option requires an argument (e.g. "
            "--j=/tmp/cfg-dbg.json)");
        return EXIT_FAILURE;
      }
      SPDLOG_DEBUG("Json Configuration: {}", config.json_configuration_path);
      Utils::RemoveArgument(config.view.vm_args,
                            "--j=" + config.json_configuration_path);
    }
    if (cl.HasOption("a")) {
      std::string accessibility_feature_flag_str;
      cl.GetOptionValue("a", &accessibility_feature_flag_str);
      if (accessibility_feature_flag_str.empty()) {
        spdlog::critical(
            "--a option (Accessibility Features) requires an "
            "argument (e.g. --a=31)");
        return EXIT_FAILURE;
      }
      int ret = 0;
      try {
        // The following styles are acceptable:
        // 1. decimal: --a=31
        // 2. hex: --a=0x3
        // 3. octet: --a=03
        config.view.accessibility_features = static_cast<int32_t>(
            std::stol(accessibility_feature_flag_str, nullptr, 0));
      } catch (const std::invalid_argument& /* e */) {
        spdlog::critical(
            "--a option (Accessibility Features) requires an integer value");
        ret = EXIT_FAILURE;
      } catch (const std::out_of_range& /* e */) {
        spdlog::critical(
            "The specified value to --a option, {} is out of range.",
            accessibility_feature_flag_str);
        ret = EXIT_FAILURE;
      }
      if (ret) {
        return ret;
      }
      config.view.accessibility_features =
          Configuration::MaskAccessibilityFeatures(
              config.view.accessibility_features);
      Utils::RemoveArgument(config.view.vm_args,
                            "--a=" + accessibility_feature_flag_str);
    }
    if (cl.HasOption("b")) {
      cl.GetOptionValue("b", &config.view.bundle_path);
      if (config.view.bundle_path.empty() ||
          !std::filesystem::is_directory(config.view.bundle_path)) {
        spdlog::critical(
            "--b (Bundle Path) option requires a directory path "
            "argument (e.g. "
            "--b=/usr/share/gallery)");
        return EXIT_FAILURE;
      }
      SPDLOG_DEBUG("Bundle Path: {}", config.view.bundle_path);
      Utils::RemoveArgument(config.view.vm_args,
                            "--b=" + config.view.bundle_path);
    }
    if (cl.HasOption("c")) {
      SPDLOG_DEBUG("Disable Cursor");
      config.disable_cursor = true;
      config.disable_cursor_set = true;
      Utils::RemoveArgument(config.view.vm_args, "--c");
    }
    if (cl.HasOption("d")) {
      SPDLOG_DEBUG("Backend Debug");
      config.debug_backend = true;
      config.debug_backend_set = true;
      Utils::RemoveArgument(config.view.vm_args, "--d");
    }
    if (cl.HasOption("f")) {
      SPDLOG_DEBUG("Fullscreen");
      config.view.fullscreen = true;
      config.view.fullscreen_set = true;
      Utils::RemoveArgument(config.view.vm_args, "--f");
    }
    if (cl.HasOption("w")) {
      std::string width_str;
      cl.GetOptionValue("w", &width_str);
      if (!Utils::IsNumber(width_str)) {
        spdlog::critical("--w option (Width) requires an integer value");
        return EXIT_FAILURE;
      }
      if (width_str.empty()) {
        spdlog::critical(
            "--w option (Width) requires an argument (e.g. --w=720)");
        return EXIT_FAILURE;
      }
      config.view.width = static_cast<uint32_t>(std::stoul(width_str));
      Utils::RemoveArgument(config.view.vm_args, "--w=" + width_str);
    }
    if (cl.HasOption("h")) {
      std::string height_str;
      cl.GetOptionValue("h", &height_str);
      if (!Utils::IsNumber(height_str)) {
        spdlog::critical("--h option (Height) requires an integer value");
        return EXIT_FAILURE;
      }
      if (height_str.empty()) {
        spdlog::critical(
            "--h option (Height) requires an argument (e.g. --w=1280)");
        return EXIT_FAILURE;
      }
      config.view.height = static_cast<uint32_t>(std::stoul(height_str));
      Utils::RemoveArgument(config.view.vm_args, "--h=" + height_str);
    }
    if (cl.HasOption("t")) {
      cl.GetOptionValue("t", &config.cursor_theme);
      if (config.cursor_theme.empty()) {
        spdlog::critical(
            "--t option requires an argument (e.g. --t=DMZ-White)");
        return EXIT_FAILURE;
      }
      SPDLOG_DEBUG("Cursor Theme: {}", config.cursor_theme);
      Utils::RemoveArgument(config.view.vm_args, "--t=" + config.cursor_theme);
    }
    if (cl.HasOption("window-type")) {
      cl.GetOptionValue("window-type", &config.view.window_type);
      if (config.view.window_type.empty()) {
        spdlog::critical(
            "--window-type option requires an argument (e.g. "
            "--window-type=BG)");
        return EXIT_FAILURE;
      }
      SPDLOG_DEBUG("Window Type: {}", config.view.window_type);
      Utils::RemoveArgument(config.view.vm_args,
                            "--window-type=" + config.view.window_type);
    }
    if (cl.HasOption("output-index")) {
      std::string output_index_str;
      cl.GetOptionValue("output-index", &output_index_str);
      if (!Utils::IsNumber(output_index_str)) {
        spdlog::critical(
            "--output-index option (Wayland Output Index) "
            "requires an integer value");
        return EXIT_FAILURE;
      }
      if (output_index_str.empty()) {
        spdlog::critical(
            "--output-index option (Wayland Output Index) "
            "requires an argument (e.g. --output-index=1)");
        return EXIT_FAILURE;
      }
      config.view.wl_output_index =
          static_cast<uint32_t>(std::stoul(output_index_str));
      Utils::RemoveArgument(config.view.vm_args,
                            "--output-index=" + output_index_str);
    }
    if (cl.HasOption("xdg-shell-app-id")) {
      cl.GetOptionValue("xdg-shell-app-id", &config.app_id);
      if (config.app_id.empty()) {
        spdlog::critical(
            "--xdg-shell-app-id option requires an argument "
            "(e.g. --xdg-shell-app-id=gallery)");
        return EXIT_FAILURE;
      }
      SPDLOG_DEBUG("Application ID: {}", config.app_id);
      Utils::RemoveArgument(config.view.vm_args,
                            "--xdg-shell-app-id=" + config.app_id);
    }
    if (cl.HasOption("wayland-event-mask")) {
      cl.GetOptionValue("wayland-event-mask", &config.wayland_event_mask);
      if (config.wayland_event_mask.empty()) {
        spdlog::critical(
            "--wayland-event-mask option requires an argument "
            "(e.g. --wayland-event-mask=pointer-axis,keyboard)");
        return EXIT_FAILURE;
      }
      SPDLOG_DEBUG("Wayland Event Mask: {}", config.wayland_event_mask);
      Utils::RemoveArgument(config.view.vm_args, "--wayland-event-mask=" +
                                                     config.wayland_event_mask);
    }
    if (cl.HasOption("p")) {
      std::string pixel_ratio_str;
      cl.GetOptionValue("p", &pixel_ratio_str);
      if (pixel_ratio_str.empty()) {
        spdlog::critical(
            "--p option (Pixel Ratio) requires an argument "
            "(e.g. --p=1.1234)");
        return EXIT_FAILURE;
      }

      config.view.pixel_ratio = strtod(pixel_ratio_str.c_str(), nullptr);
      Utils::RemoveArgument(config.view.vm_args, "--p=" + pixel_ratio_str);
    }
    if (cl.HasOption("i")) {
      std::string ivi_surface_id_str;
      cl.GetOptionValue("i", &ivi_surface_id_str);
      if (ivi_surface_id_str.empty()) {
        spdlog::critical(
            "--i option (IVI Surface ID) requires an argument "
            "(e.g. --i=2)");
        return EXIT_FAILURE;
      }

      config.view.ivi_surface_id = static_cast<uint32_t>(
          strtoul(ivi_surface_id_str.c_str(), nullptr, 10));
      Utils::RemoveArgument(config.view.vm_args, "--i=" + ivi_surface_id_str);
    }
  } else {
    PrintUsage();
    exit(EXIT_SUCCESS);
  }
  return EXIT_SUCCESS;
}

int32_t Configuration::MaskAccessibilityFeatures(
    int32_t accessibility_features) {
  accessibility_features &= 0b1111111;
  return accessibility_features;
}
