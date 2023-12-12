/*
 * Copyright 2020-2023 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "url_launcher_plugin.h"

#include "messages.g.h"

#include <flutter/plugin_registrar.h>

#include <memory>
#include <sstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace url_launcher_linux {

// static
void UrlLauncherPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<UrlLauncherPlugin>();

  UrlLauncherApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

UrlLauncherPlugin::UrlLauncherPlugin() = default;

UrlLauncherPlugin::~UrlLauncherPlugin() = default;

ErrorOr<bool> UrlLauncherPlugin::CanLaunchUrl(const std::string& url) {
  if (url.find(':') == std::string::npos) {
    return false;
  }

  return (url.rfind("https:", 0) == 0) || (url.rfind("http:", 0) == 0) ||
         (url.rfind("ftp:", 0) == 0) || (url.rfind("file:", 0) == 0);
}

std::optional<FlutterError> UrlLauncherPlugin::LaunchUrl(const std::string& url) {
  const pid_t pid = fork();
  if (pid == 0) {
    execl("/usr/bin/xdg-open", "xdg-open", url.c_str(), nullptr);
    exit(1);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (status != 0) {
    std::ostringstream error_message;
    error_message << "Failed to open " << url << ": error " << status;
    return FlutterError("open_error", error_message.str());
  }

  return std::nullopt;
}

}  // namespace url_launcher_linux
