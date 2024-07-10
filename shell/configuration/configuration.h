// Copyright 2022 Toyota Connected North America
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

#pragma once

#include <string>
#include <vector>

#define TOML_EXCEPTIONS 0
#include <tomlplusplus/toml.hpp>

#include "utils.h"

class Configuration {
 public:
  struct Config {
    std::string app_id;
    std::string json_configuration_path;
    std::string cursor_theme;
    bool disable_cursor;
    bool disable_cursor_set;
    std::string wayland_event_mask;
    bool debug_backend;
    bool debug_backend_set;
    std::vector<std::string> bundle_paths;

    struct {
      std::string bundle_path;
      std::vector<std::string> vm_args;
      std::string window_type;
      uint32_t wl_output_index;
      int32_t accessibility_features;
      uint32_t width;
      uint32_t height;
      uint32_t activation_area_x;
      uint32_t activation_area_y;
      uint32_t activation_area_width;
      uint32_t activation_area_height;
      bool fullscreen;
      bool fullscreen_set;
      double pixel_ratio;
      uint32_t ivi_surface_id;
      uint32_t fps_output_console;
      uint32_t fps_output_overlay;
      uint32_t fps_output_frequency;
    } view;
  };

  /**
   * @brief config file generate from argc and argv
   * @param[in] argc argument count
   * @param[in] argv argument vector
   * @return Config
   * @retval generated config object
   * @relation
   * internal
   */
  static std::vector<Config> ParseArgcArgv(int argc, const char* const* argv);

  /**
   * @brief Print the contents of the configuration to the log
   * @param[in] config Pointer to config object to print
   * @return void
   * @relation
   * internal
   */
  static void PrintConfig(const Config& config);

  Configuration(const Configuration&) = delete;
  Configuration& operator=(const Configuration&) = delete;

 private:
  /**
   * @brief Parse config file and generate View config
   * @param[in] cli_config Config file
   * @return std::vector<Configuration::Config>
   * @retval View config
   * @relation
   * internal
   */
  static std::vector<Config> parse_config(const Config& cli_config);

  /**
   * @brief Get parameters from TOML configuration file
   * @param[in] tbl TOML table
   * @param[in,out] instance config
   * @return void
   * @relation
   * internal
   */
  static void get_parameters(toml::table* tbl, Config& instance);

  /**
   * @brief Get Doc parameters set to View config
   * @param[in] config_toml_path path of config.toml file
   * @param[in,out] instance View config
   * @return void
   * @relation
   * internal
   */
  static void get_toml_config(const char* config_toml_path, Config& instance);

  /**
   * @brief Get Cli config overrides to View config
   * @param[in] bundle_path bundle_path
   * @param[in,out] instance View config
   * @param[in] cli Cli config
   * @return void
   * @relation
   * internal
   */
  static void get_cli_override(const std::string& bundle_path,
                               Config& instance,
                               const Config& cli);

  /**
   * @brief mask the accessibility_features
   * @param[in] accessibility_features accessibility_features value
   * @return int32_t
   * @retval masked accessibility_features value
   * @relation internal
   *
   * accessibility_features is expressed as bit flags.
   * please see FlutterAccessibilityFeature enum
   * in third_party/flutter/shell/platform/embedder/embedder.h.
   * 0b1111111 is the maximum value of accessibility_features.
   */
  static int32_t mask_accessibility_features(int32_t accessibility_features);
};
