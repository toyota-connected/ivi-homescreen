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
#include <sstream>

#include "app.h"
#include "configuration/configuration.h"

#include <flutter/fml/command_line.h>
#include <flutter/fml/logging.h>

volatile bool running = true;

void SignalHandler(int signal) {
  (void)signal;
  FML_DLOG(INFO) << "Ctl+C";
  running = false;
}

bool IsNumber(const std::string& s) {
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return isdigit(c) != 0; });
}

void RemoveArgument(std::vector<std::string>& args, const std::string& arg) {
  auto result = std::find(args.begin(), args.end(), arg);
  if (result != args.end()) {
    args.erase(result);
  }
}

int main(int argc, char** argv) {
  struct Configuration::Config config {
    .app_id = kApplicationName, .json_configuration_path{}, .cursor_theme{},
    .disable_cursor{}, .debug_backend{}, .view {}
  };

  config.view.vm_args.reserve(argc - 1);
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
      if (config.view.bundle_path.empty()) {
        FML_LOG(ERROR) << "--b (Bundle Path) option requires an argument (e.g. "
                          "--b=/usr/share/gallery)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Bundle Path: " << config.view.bundle_path;
      RemoveArgument(config.view.vm_args, "--b=" + config.view.bundle_path);
    }
    if (cl.HasOption("c")) {
      FML_DLOG(INFO) << "Disable Cursor";
      config.disable_cursor = true;
      RemoveArgument(config.view.vm_args, "--c");
    }
    if (cl.HasOption("d")) {
      FML_DLOG(INFO) << "Backend Debug";
      config.debug_backend = true;
      RemoveArgument(config.view.vm_args, "--d");
    }
    if (cl.HasOption("f")) {
      FML_DLOG(INFO) << "Fullscreen";
      config.view.fullscreen = true;
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
        FML_LOG(ERROR)
            << "--window-type option requires an argument (e.g. --window-type=BG)";
        return EXIT_FAILURE;
      }
      FML_DLOG(INFO) << "Window Type: " << config.view.window_type;
      RemoveArgument(config.view.vm_args, "--window-type=" + config.view.window_type);
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
