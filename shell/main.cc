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
  bool fullscreen = false;
  bool show_cursor = true;
  bool debug_egl = false;

  auto cl = fml::CommandLineFromArgcArgv(argc, argv);

  if (!cl.options().empty()) {
    if (cl.HasOption("fullscreen") || cl.HasOption("f")) {
      fullscreen = true;
    }
  }

  App app("homescreen", fullscreen, show_cursor, debug_egl);

  std::signal(SIGINT, SignalHandler);

  // run the application
  int ret = 0;
  while (running && ret != -1) {
    ret = app.Loop();
  }

  return 0;
}
