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

#include "rapidjson/rapidjson.h"

#include "rapidjson/document.h"

#include "cxxopts/include/cxxopts.hpp"
#include "flutter/fml/macros.h"

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

    struct {
      std::vector<std::string> vm_args;
      std::string bundle_path;
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
   * @brief Parse config file and generate View config
   * @param[in] config Config file
   * @return std::vector<Configuration::Config>
   * @retval View config
   * @relation
   * internal
   */
  static std::vector<struct Configuration::Config> ParseConfig(
      const Config& config);

  /**
   * @brief Print the contents of the configuration to the log
   * @param[in] config Pointer to config object to print
   * @return void
   * @relation
   * internal
   */
  static void PrintConfig(const Config& config);

  /**
   * @brief config file generate from argc and argv
   * @param[in] argc argument count
   * @param[in] argv argument vector
   * @return Config
   * @retval generated config object
   * @relation
   * internal
   */
  static Config ParseArgcArgv(int argc, const char* const* argv);

  FML_DISALLOW_COPY_AND_ASSIGN(Configuration);

 private:
  static constexpr char kViewKey[] = "view";
  static constexpr char kBundlePathKey[] = "bundle_path";
  static constexpr char kWindowTypeKey[] = "window_type";
  static constexpr char kOutputIndex[] = "output_index";
  static constexpr char kWindowActivationAreaKey[] = "window_activation_area";
  static constexpr char kWidthKey[] = "width";
  static constexpr char kHeightKey[] = "height";
  static constexpr char kPixelRatioKey[] = "pixel_ratio";
  static constexpr char kIviSurfaceIdKey[] = "ivi_surface_id";
  static constexpr char kAccessibilityFeaturesKey[] = "accessibility_features";
  static constexpr char kVmArgsKey[] = "vm_args";
  static constexpr char kFullscreenKey[] = "fullscreen";
  static constexpr char kAppIdKey[] = "app_id";
  static constexpr char kCursorThemeKey[] = "cursor_theme";
  static constexpr char kWaylandEventMaskKey[] = "wayland_event_mask";
  static constexpr char kDisableCursorKey[] = "disable_cursor";
  static constexpr char kDebugBackendKey[] = "debug_backend";
  static constexpr char kFpsOutputConsole[] = "fps_output_console";
  static constexpr char kFpsOutputOverlay[] = "fps_output_overlay";
  static constexpr char kFpsOutputFrequency[] = "fps_output_frequency";

  /**
   * @brief Parse a Json Document string into DOM
   * @param[in] filename Json Document path
   * @return rapidjson::Document
   * @retval Document Object
   * @relation
   * internal
   */
  static rapidjson::Document getJsonDocument(const std::string& filename);

  /**
   * @brief Get View counts
   * @param[in] doc Document Object
   * @return rapidjson::SizeType
   * @retval Number of element counts
   * @relation
   * internal
   */
  static rapidjson::SizeType getViewCount(rapidjson::Document& doc);

  /**
   * @brief Get View parameters set to View config
   * @param[in] obj Conifg parameters
   * @param[in,out] instance View config
   * @return void
   * @relation
   * internal
   */
  static void getViewParameters(
      const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
      Config& instance);

  /**
   * @brief Get Global parameters set to View config
   * @param[in] obj Conifg parameters
   * @param[in,out] instance View confige
   * @return void
   * @relation
   * internal
   */
  static void getGlobalParameters(
      const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
      Config& instance);

  /**
   * @brief Get Doc parameters set to View config
   * @param[in] doc Document Object
   * @param[in] index 0 ~ View counts-1
   * @param[in,out] instance View config
   * @return void
   * @relation
   * internal
   */
  static void getView(rapidjson::Document& doc, int index, Config& instance);

  /**
   * @brief Get Cli config overrides to View config
   * @param[in,out] instance View config
   * @param[in] cli Cli config
   * @return void
   * @relation
   * internal
   */
  static void getCliOverrides(Config& instance, const Config& cli);

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
  static int32_t MaskAccessibilityFeatures(int32_t accessibility_features);
};
