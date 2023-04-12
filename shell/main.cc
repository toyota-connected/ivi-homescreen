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

#include <flutter/fml/command_line.h>
#include <filesystem>

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
  FML_DLOG(INFO) << "Ctl+C";
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
        .disable_cursor_set{}, .debug_backend{}, .debug_backend_set{}, .view{},
  };

  FML_LOG(INFO) << GIT_BRANCH " @ " GIT_HASH;

  config.view.vm_args.reserve(static_cast<unsigned long>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    config.view.vm_args.emplace_back(argv[i]);
  }

  auto cl = fml::CommandLineFromArgcArgv(argc, argv);

  if (!cl.options().empty()) {
    if (cl.HasOption("j")) {
      cl.GetOptionValue("j", &config.json_configuration_path);
      if (config.json_configuration_path.empty()) {
        FML_LOG(ERROR) << "--j option requires an argument (e.g. "
                          "--j=/tmp/cfg-dbg.json)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Json Configuration: "
                     << config.json_configuration_path;
      RemoveArgument(config.view.vm_args,
                     "--j=" + config.json_configuration_path);
    }
    if (cl.HasOption("a")) {
      std::string accessibility_feature_flag_str;
      cl.GetOptionValue("a", &accessibility_feature_flag_str);
      if (!IsNumber(accessibility_feature_flag_str)) {
        FML_LOG(ERROR)
            << "--a option (Accessibility Features) requires an integer value";
        return EXIT_FAILURE;
      }
      if (accessibility_feature_flag_str.empty()) {
        FML_LOG(ERROR) << "--a option (Accessibility Features) requires an "
                          "argument (e.g. --a=31)";
        return EXIT_FAILURE;
      }
      config.view.accessibility_features =
          static_cast<int32_t>(std::stol(accessibility_feature_flag_str));
      RemoveArgument(config.view.vm_args,
                     "--a=" + accessibility_feature_flag_str);
    }
    if (cl.HasOption("b")) {
      cl.GetOptionValue("b", &config.view.bundle_path);
      if (config.view.bundle_path.empty() || !std::filesystem::is_directory(config.view.bundle_path)) {
        FML_LOG(ERROR) << "--b (Bundle Path) option requires a directory path argument (e.g. "
                          "--b=/usr/share/gallery)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Bundle Path: " << config.view.bundle_path;
      RemoveArgument(config.view.vm_args, "--b=" + config.view.bundle_path);
    }
    if (cl.HasOption("c")) {
      FML_DLOG(INFO) << "Disable Cursor";
      config.disable_cursor = true;
      config.disable_cursor_set = true;
      RemoveArgument(config.view.vm_args, "--c");
    }
    if (cl.HasOption("d")) {
      FML_DLOG(INFO) << "Backend Debug";
      config.debug_backend = true;
      config.debug_backend_set = true;
      RemoveArgument(config.view.vm_args, "--d");
    }
    if (cl.HasOption("f")) {
      FML_DLOG(INFO) << "Fullscreen";
      config.view.fullscreen = true;
      config.view.fullscreen_set = true;
      RemoveArgument(config.view.vm_args, "--f");
    }
    if (cl.HasOption("w")) {
      std::string width_str;
      cl.GetOptionValue("w", &width_str);
      if (!IsNumber(width_str)) {
        FML_LOG(ERROR) << "--w option (Width) requires an integer value";
        return EXIT_FAILURE;
      }
      if (width_str.empty()) {
        FML_LOG(ERROR)
            << "--w option (Width) requires an argument (e.g. --w=720)";
        return EXIT_FAILURE;
      }
      config.view.width = static_cast<uint32_t>(std::stoul(width_str));
      RemoveArgument(config.view.vm_args, "--w=" + width_str);
    }
    if (cl.HasOption("h")) {
      std::string height_str;
      cl.GetOptionValue("h", &height_str);
      if (!IsNumber(height_str)) {
        FML_LOG(ERROR) << "--h option (Height) requires an integer value";
        return EXIT_FAILURE;
      }
      if (height_str.empty()) {
        FML_LOG(ERROR)
            << "--h option (Height) requires an argument (e.g. --w=1280)";
        return EXIT_FAILURE;
      }
      config.view.height = static_cast<uint32_t>(std::stoul(height_str));
      RemoveArgument(config.view.vm_args, "--h=" + height_str);
    }
    if (cl.HasOption("t")) {
      cl.GetOptionValue("t", &config.cursor_theme);
      if (config.cursor_theme.empty()) {
        FML_LOG(ERROR)
            << "--t option requires an argument (e.g. --t=DMZ-White)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Cursor Theme: " << config.cursor_theme;
      RemoveArgument(config.view.vm_args, "--t=" + config.cursor_theme);
    }
    if (cl.HasOption("window-type")) {
      cl.GetOptionValue("window-type", &config.view.window_type);
      if (config.view.window_type.empty()) {
        FML_LOG(ERROR) << "--window-type option requires an argument (e.g. "
                          "--window-type=BG)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Window Type: " << config.view.window_type;
      RemoveArgument(config.view.vm_args,
                     "--window-type=" + config.view.window_type);
    }
    if (cl.HasOption("output-index")) {
      std::string output_index_str;
      cl.GetOptionValue("output-index", &output_index_str);
      if (!IsNumber(output_index_str)) {
        FML_LOG(ERROR) << "--output-index option (Wayland Output Index) "
                          "requires an integer value";
        return EXIT_FAILURE;
      }
      if (output_index_str.empty()) {
        FML_LOG(ERROR) << "--output-index option (Wayland Output Index) "
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
        FML_LOG(ERROR) << "--xdg-shell-app-id option requires an argument "
                          "(e.g. --xdg-shell-app-id=gallery)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Application ID: " << config.app_id;
      RemoveArgument(config.view.vm_args,
                     "--xdg-shell-app-id=" + config.app_id);
    }
    if (cl.HasOption("p")) {
      std::string pixel_ratio_str;
      cl.GetOptionValue("p", &pixel_ratio_str);
      if (pixel_ratio_str.empty()) {
        FML_LOG(ERROR) << "--p option (Pixel Ratio) requires an argument "
                          "(e.g. --p=1.1234)";
        return EXIT_FAILURE;
      }

      char* ptr;
      config.view.pixel_ratio = strtod(pixel_ratio_str.c_str(), &ptr);
      RemoveArgument(config.view.vm_args, "--p=" + pixel_ratio_str);
    }
  }

  auto vm_arg_count = config.view.vm_args.size();
  if (vm_arg_count) {
    FML_DLOG(INFO) << "VM Arg Count: " << vm_arg_count;
    for (auto const& arg : config.view.vm_args) {
      FML_DLOG(INFO) << arg;
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

  return EXIT_SUCCESS;
}
