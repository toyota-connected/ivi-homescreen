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

#ifndef FLUTTER_PLUGIN_URL_LAUNCHER_PLUGIN_H
#define FLUTTER_PLUGIN_URL_LAUNCHER_PLUGIN_H

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.g.h"

namespace url_launcher_linux {

class UrlLauncherPlugin final : public flutter::Plugin, public UrlLauncherApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  UrlLauncherPlugin();

  ~UrlLauncherPlugin() override;

  // Disallow copy and assign.
  UrlLauncherPlugin(const UrlLauncherPlugin&) = delete;
  UrlLauncherPlugin& operator=(const UrlLauncherPlugin&) = delete;

  // UrlLauncherApi methods.
  ErrorOr<bool> CanLaunchUrl(const std::string& url) override;
  std::optional<FlutterError> LaunchUrl(const std::string& url) override;
};

}  // namespace url_launcher_linux

#endif  // FLUTTER_PLUGIN_URL_LAUNCHER_PLUGIN_H
