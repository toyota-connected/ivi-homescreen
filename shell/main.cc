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

#include <flutter/fml/command_line.h>
#include <flutter/fml/logging.h>

volatile bool running = true;

void SignalHandler([[maybe_unused]] int signal) {
  FML_DLOG(INFO) << "Ctl+C";
  running = false;
}

int main(int argc, char** argv) {
  std::vector<std::string> args;
#ifndef NDEBUG
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
#endif

  std::string application_override_path;
  bool disable_cursor = false;
  bool debug_egl = false;
  bool fullscreen = false;

  auto cl = fml::CommandLineFromArgcArgv(argc, argv);

  if (!cl.options().empty()) {
    if (cl.HasOption("a")) {
      cl.GetOptionValue("a", &application_override_path);
      FML_DLOG(INFO) << "Override Assets Path: "
                     << application_override_path;
      auto find = "--a=" + application_override_path;
      auto result = std::find(args.begin(), args.end(), find);
      if (result != args.end()) {
        args.erase(result);
      }
    }
    if (cl.HasOption("c")) {
      FML_DLOG(INFO) << "Disable Cursor";
      disable_cursor = true;
      auto result = std::find(args.begin(), args.end(), "--c");
      if (result != args.end()) {
        args.erase(result);
      }
    }
#ifndef NDEBUG
    if (cl.HasOption("e")) {
      FML_DLOG(INFO) << "EGL Debug";
      debug_egl = true;
      auto result = std::find(args.begin(), args.end(), "--e");
      if (result != args.end()) {
        args.erase(result);
      }
    }
#endif
    if (cl.HasOption("f")) {
      FML_DLOG(INFO) << "Fullscreen";
      fullscreen = true;
      auto idx = std::find(args.begin(), args.end(), "--f");
      if (idx != args.end()) {
        args.erase(idx);
      }
    }
  }

  App app("homescreen", args, application_override_path, fullscreen,
          !disable_cursor, debug_egl);

  std::signal(SIGINT, SignalHandler);

  // run the application
  int ret = 0;
  while (running && ret != -1) {
    ret = app.Loop();
  }

  return 0;
}
