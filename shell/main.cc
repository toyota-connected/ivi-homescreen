// Copyright 2020 Toyota Connected North America
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

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <sstream>

#include "app.h"
#include "configuration/configuration.h"
#include "logging/logging.h"

#include <flutter/fml/command_line.h>
#include <filesystem>

#if defined(BUILD_CRASH_HANDLER)
#include "crash_handler.h"
#endif

volatile bool running = true;

/**
 * @brief Signal handler
 * @param[in] signal No use
 * @return void
 * @relation
 * internal
 */
void SignalHandler(int signal) {
  (void)signal;
  DLOG(INFO) << "Ctl+C";
  running = false;
}

/**
 * @brief Check if input is a number
 * @param[in] s String to check if it is a number
 * @return bool
 * @retval true If s is a number
 * @retval false If s is not a number
 * @relation
 * internal
 */
bool IsNumber(const std::string& s) {
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return isdigit(c) != 0; });
}

/**
 * @brief Remove argument from vector
 * @param[in] args Vector of element that matches the argument to be removed
 * @param[in] arg Arguments to be removed
 * @return void
 * @relation
 * internal
 */
void RemoveArgument(std::vector<std::string>& args, const std::string& arg) {
  auto result = std::find(args.begin(), args.end(), arg);
  if (result != args.end()) {
    args.erase(result);
  }
}

/**
 * @brief Main function
 * @param[in] argc Number of arguments
 * @param[in] argv Arguments passed to the program
 * @return int
 * @retval 0 Normal end
 * @retval Non-zero Abnormal end
 * @relation
 * wayland, flutter
 */
int main(int argc, char** argv) {
  struct Configuration::Config config {
    .app_id{}, .json_configuration_path{}, .cursor_theme{}, .disable_cursor{},
        .disable_cursor_set{}, .wayland_event_mask{}, .debug_backend{},
        .debug_backend_set{}, .view{},
  };

#if defined(BUILD_CRASH_HANDLER)
  auto crash_handler = std::make_unique<CrashHandler>();
#endif

  if (Dlt::IsSupported()) {
    Dlt::Register();
  }

  LOG(INFO) << GIT_BRANCH " @ " GIT_HASH;

  config.view.vm_args.reserve(static_cast<unsigned long>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    config.view.vm_args.emplace_back(argv[i]);
  }

  auto cl = fml::CommandLineFromArgcArgv(argc, argv);

  if (!cl.options().empty()) {
    if (cl.HasOption("j")) {
      cl.GetOptionValue("j", &config.json_configuration_path);
      if (config.json_configuration_path.empty()) {
        LOG(ERROR) << "--j option requires an argument (e.g. "
                      "--j=/tmp/cfg-dbg.json)";
        return EXIT_FAILURE;
      }
      DLOG(INFO) << "Json Configuration: " << config.json_configuration_path;
      RemoveArgument(config.view.vm_args,
                     "--j=" + config.json_configuration_path);
    }
    if (cl.HasOption("a")) {
      std::string accessibility_feature_flag_str;
      cl.GetOptionValue("a", &accessibility_feature_flag_str);
      if (accessibility_feature_flag_str.empty()) {
        LOG(ERROR) << "--a option (Accessibility Features) requires an "
                      "argument (e.g. --a=31)";
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
      } catch (const std::invalid_argument& e) {
        LOG(ERROR)
            << "--a option (Accessibility Features) requires an integer value";
        ret = EXIT_FAILURE;
      } catch (const std::out_of_range& e) {
        LOG(ERROR) << "The specified value to --a option, "
                   << accessibility_feature_flag_str << " is out of range.";
        ret = EXIT_FAILURE;
      }
      if (ret) {
        return ret;
      }
      config.view.accessibility_features =
          Configuration::MaskAccessibilityFeatures(
              config.view.accessibility_features);
      RemoveArgument(config.view.vm_args,
                     "--a=" + accessibility_feature_flag_str);
    }
    if (cl.HasOption("b")) {
      cl.GetOptionValue("b", &config.view.bundle_path);
      if (config.view.bundle_path.empty() ||
          !std::filesystem::is_directory(config.view.bundle_path)) {
        LOG(ERROR) << "--b (Bundle Path) option requires a directory path "
                      "argument (e.g. "
                      "--b=/usr/share/gallery)";
        return EXIT_FAILURE;
      }
      DLOG(INFO) << "Bundle Path: " << config.view.bundle_path;
      RemoveArgument(config.view.vm_args, "--b=" + config.view.bundle_path);
    }
    if (cl.HasOption("c")) {
      DLOG(INFO) << "Disable Cursor";
      config.disable_cursor = true;
      config.disable_cursor_set = true;
      RemoveArgument(config.view.vm_args, "--c");
    }
    if (cl.HasOption("d")) {
      DLOG(INFO) << "Backend Debug";
      config.debug_backend = true;
      config.debug_backend_set = true;
      RemoveArgument(config.view.vm_args, "--d");
    }
    if (cl.HasOption("f")) {
      DLOG(INFO) << "Fullscreen";
      config.view.fullscreen = true;
      config.view.fullscreen_set = true;
      RemoveArgument(config.view.vm_args, "--f");
    }
    if (cl.HasOption("w")) {
      std::string width_str;
      cl.GetOptionValue("w", &width_str);
      if (!IsNumber(width_str)) {
        LOG(ERROR) << "--w option (Width) requires an integer value";
        return EXIT_FAILURE;
      }
      if (width_str.empty()) {
        LOG(ERROR) << "--w option (Width) requires an argument (e.g. --w=720)";
        return EXIT_FAILURE;
      }
      config.view.width = static_cast<uint32_t>(std::stoul(width_str));
      RemoveArgument(config.view.vm_args, "--w=" + width_str);
    }
    if (cl.HasOption("h")) {
      std::string height_str;
      cl.GetOptionValue("h", &height_str);
      if (!IsNumber(height_str)) {
        LOG(ERROR) << "--h option (Height) requires an integer value";
        return EXIT_FAILURE;
      }
      if (height_str.empty()) {
        LOG(ERROR)
            << "--h option (Height) requires an argument (e.g. --w=1280)";
        return EXIT_FAILURE;
      }
      config.view.height = static_cast<uint32_t>(std::stoul(height_str));
      RemoveArgument(config.view.vm_args, "--h=" + height_str);
    }
    if (cl.HasOption("t")) {
      cl.GetOptionValue("t", &config.cursor_theme);
      if (config.cursor_theme.empty()) {
        LOG(ERROR) << "--t option requires an argument (e.g. --t=DMZ-White)";
        return EXIT_FAILURE;
      }
      DLOG(INFO) << "Cursor Theme: " << config.cursor_theme;
      RemoveArgument(config.view.vm_args, "--t=" + config.cursor_theme);
    }
    if (cl.HasOption("window-type")) {
      cl.GetOptionValue("window-type", &config.view.window_type);
      if (config.view.window_type.empty()) {
        LOG(ERROR) << "--window-type option requires an argument (e.g. "
                      "--window-type=BG)";
        return EXIT_FAILURE;
      }
      DLOG(INFO) << "Window Type: " << config.view.window_type;
      RemoveArgument(config.view.vm_args,
                     "--window-type=" + config.view.window_type);
    }
    if (cl.HasOption("output-index")) {
      std::string output_index_str;
      cl.GetOptionValue("output-index", &output_index_str);
      if (!IsNumber(output_index_str)) {
        LOG(ERROR) << "--output-index option (Wayland Output Index) "
                      "requires an integer value";
        return EXIT_FAILURE;
      }
      if (output_index_str.empty()) {
        LOG(ERROR) << "--output-index option (Wayland Output Index) "
                      "requires an argument (e.g. --output-index=1)";
        return EXIT_FAILURE;
      }
      config.view.wl_output_index =
          static_cast<uint32_t>(std::stoul(output_index_str));
      RemoveArgument(config.view.vm_args, "--output-index=" + output_index_str);
    }
    if (cl.HasOption("xdg-shell-app-id")) {
      cl.GetOptionValue("xdg-shell-app-id", &config.app_id);
      if (config.app_id.empty()) {
        LOG(ERROR) << "--xdg-shell-app-id option requires an argument "
                      "(e.g. --xdg-shell-app-id=gallery)";
        return EXIT_FAILURE;
      }
      DLOG(INFO) << "Application ID: " << config.app_id;
      RemoveArgument(config.view.vm_args,
                     "--xdg-shell-app-id=" + config.app_id);
    }
    if (cl.HasOption("wayland-event-mask")) {
      cl.GetOptionValue("wayland-event-mask", &config.wayland_event_mask);
      if (config.wayland_event_mask.empty()) {
        LOG(ERROR) << "--wayland-event-mask option requires an argument "
                      "(e.g. --wayland-event-mask=pointer-axis,keyboard)";
        return EXIT_FAILURE;
      }
      DLOG(INFO) << "Wayland Event Mask: " << config.wayland_event_mask;
      RemoveArgument(config.view.vm_args,
                     "--wayland-event-mask=" + config.wayland_event_mask);
    }
    if (cl.HasOption("p")) {
      std::string pixel_ratio_str;
      cl.GetOptionValue("p", &pixel_ratio_str);
      if (pixel_ratio_str.empty()) {
        LOG(ERROR) << "--p option (Pixel Ratio) requires an argument "
                      "(e.g. --p=1.1234)";
        return EXIT_FAILURE;
      }

      config.view.pixel_ratio = strtod(pixel_ratio_str.c_str(), nullptr);
      RemoveArgument(config.view.vm_args, "--p=" + pixel_ratio_str);
    }
    if (cl.HasOption("i")) {
      std::string ivi_surface_id_str;
      cl.GetOptionValue("i", &ivi_surface_id_str);
      if (ivi_surface_id_str.empty()) {
        LOG(ERROR) << "--i option (IVI Surface ID) requires an argument "
                      "(e.g. --i=2)";
        return EXIT_FAILURE;
      }

      config.view.ivi_surface_id = static_cast<uint32_t>(
          strtoul(ivi_surface_id_str.c_str(), nullptr, 10));
      RemoveArgument(config.view.vm_args, "--i=" + ivi_surface_id_str);
    }
  }

  auto vm_arg_count = config.view.vm_args.size();
  if (vm_arg_count) {
    DLOG(INFO) << "VM Arg Count: " << vm_arg_count;
    for (auto const& arg : config.view.vm_args) {
      DLOG(INFO) << arg;
    }
  }

  auto configs = Configuration::ParseConfig(config);
  for (auto const& c : configs) {
    Configuration::PrintConfig(c);
  }
  assert(!configs.empty());

  App app(configs);

  std::signal(SIGINT, SignalHandler);

  // run the application
  int ret = 0;
  while (running && ret != -1) {
    ret = app.Loop();
  }

  if (Dlt::IsSupported()) {
    Dlt::Unregister();
  }

#if defined(BUILD_CRASH_HANDLER)
  (void)crash_handler.release();
#endif

  return EXIT_SUCCESS;
}
