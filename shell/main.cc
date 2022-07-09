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

bool isNumber(const std::string& s) {
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return isdigit(c) != 0; });
}

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  struct Configuration::Config config {
    .app_id = kApplicationName, .json_configuration_path = "",
    .cursor_theme = "", .disable_cursor = false, .debug_backend = false, .view {
      .command_line_args{}, .bundle_path = "",
                            .window_type = "", .accessibility_features = 0,
                            .width = 0, .height = 0, .fullscreen = false
    }
  };

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
      auto find = "--j=" + config.json_configuration_path;
      auto result = std::find(args.begin(), args.end(), find);
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("a")) {
      std::string accessibility_feature_flag_str;
      cl.GetOptionValue("a", &accessibility_feature_flag_str);
      if (!isNumber(accessibility_feature_flag_str)) {
        FML_LOG(ERROR) << "--a option (Accessibility Features) requires an integer value";
        exit(EXIT_FAILURE);
      }
      if (accessibility_feature_flag_str.empty()) {
        FML_LOG(ERROR) << "--a option (Accessibility Features) requires an "
                          "argument (e.g. --a=31)";
        return 1;
      }
      config.view.accessibility_features =
          static_cast<int32_t>(std::stol(accessibility_feature_flag_str));
      auto find = "--a=" + accessibility_feature_flag_str;
      auto result = std::find(args.begin(), args.end(), find);
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("b")) {
      cl.GetOptionValue("b", &config.view.bundle_path);
      if (config.view.bundle_path.empty()) {
        FML_LOG(ERROR) << "--b (Bundle Path) option requires an argument (e.g. "
                          "--b=/usr/share/gallery)";
        return 1;
      }
      FML_DLOG(INFO) << "Bundle Path: " << config.view.bundle_path;
      auto find = "--b=" + config.view.bundle_path;
      auto result = std::find(args.begin(), args.end(), find);
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("c")) {
      FML_DLOG(INFO) << "Disable Cursor";
      config.disable_cursor = true;
      auto result = std::find(args.begin(), args.end(), "--c");
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("d")) {
      FML_DLOG(INFO) << "Backend Debug";
      config.debug_backend = true;
      auto result = std::find(args.begin(), args.end(), "--d");
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("f")) {
      FML_DLOG(INFO) << "Fullscreen";
      config.view.fullscreen = true;
      auto result = std::find(args.begin(), args.end(), "--f");
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("w")) {
      std::string width_str;
      cl.GetOptionValue("w", &width_str);
      if (!isNumber(width_str)) {
        FML_LOG(ERROR) << "--w option (Width) requires an integer value";
        exit(EXIT_FAILURE);
      }
      if (width_str.empty()) {
        FML_LOG(ERROR) << "--w option (Width) requires an argument (e.g. --w=720)";
        return 1;
      }
      config.view.width = static_cast<uint32_t>(std::stoul(width_str));
      auto find = "--w=" + width_str;
      auto result = std::find(args.begin(), args.end(), find);
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("h")) {
      std::string height_str;
      cl.GetOptionValue("h", &height_str);
      if (!isNumber(height_str)) {
        FML_LOG(ERROR) << "--h option (Height) requires an integer value";
        exit(EXIT_FAILURE);
      }
      if (height_str.empty()) {
        FML_LOG(ERROR) << "--h option (Height) requires an argument (e.g. --w=1280)";
        return 1;
      }
      config.view.height = static_cast<uint32_t>(std::stoul(height_str));
      auto find = "--h=" + height_str;
      auto result = std::find(args.begin(), args.end(), find);
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("t")) {
      cl.GetOptionValue("t", &config.cursor_theme);
      if (config.cursor_theme.empty()) {
        FML_LOG(ERROR)
            << "--t option requires an argument (e.g. --t=DMZ-White)";
        return 1;
      }
      FML_DLOG(INFO) << "Cursor Theme: " << config.cursor_theme;
      auto result = std::find(args.begin(), args.end(), "--t");
      if (result != args.end()) {
        args.erase(result);
      }
    }
  }

  auto configs = Configuration::ParseConfig(config);
  for(auto const &c : configs) {
    Configuration::PrintConfig(c);
  }
  assert(configs.capacity() > 0);

  App app(configs);

  std::signal(SIGINT, SignalHandler);

  // run the application
  int ret = 0;
  while (running && ret != -1) {
      ret = app.Loop();
  }

  return 0;
}
