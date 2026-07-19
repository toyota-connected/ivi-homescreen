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

#include <optional>
#include <string>
#include <vector>

#define TOML_EXCEPTIONS 0
#include <tomlplusplus/toml.hpp>

#include "display/output.h"
#include "utils.h"

class Configuration {
 public:
  struct Config {
    std::string app_id;
    std::string cursor_theme;
    std::optional<bool> disable_cursor;
    std::string wayland_event_mask;
    std::optional<bool> debug_backend;
    std::vector<std::string> bundle_paths;

    struct {
      std::string bundle_path;
      // Engine / Dart VM switches -> FlutterProjectArgs.command_line_argv
      // (app_id is prepended as argv[0] downstream). Unmatched CLI options land
      // here too.
      std::vector<std::string> engine_args;
      // Arguments to the Dart entrypoint main(List<String>) ->
      // FlutterProjectArgs.dart_entrypoint_argv (no argv[0]).
      std::vector<std::string> dart_args;
      std::string window_type;
      std::optional<uint32_t> wl_output_index;
      std::optional<int32_t> accessibility_features;
      std::optional<uint32_t> width;
      std::optional<uint32_t> height;
      uint32_t activation_area_x;
      uint32_t activation_area_y;
      uint32_t activation_area_width;
      uint32_t activation_area_height;
      std::optional<bool> fullscreen;
      std::optional<double> pixel_ratio;
      std::optional<uint32_t> ivi_surface_id;
      std::optional<std::string> drm_device;

      // [view.engine] merge_render_platform: run the engine's raster thread on
      // the platform thread (one FlutterCustomTaskRunners identifier for both).
      // Gives native/Dart-FFI code single-thread aliasing; the present path
      // must be merge-agnostic (non-blocking on the platform thread). Off by
      // default.
      std::optional<bool> merge_render_platform;

      // DRM backend knobs — all strings so Configuration stays decoupled
      // from the DRM headers. Valid values:
      //   drm_connector             : "<TypeName>-<id>" (e.g. "eDP-1",
      //                               "HDMI-A-1"); unset = rank-pick
      //   drm_mode                  : "<W>x<H>@<R>" (e.g. "1920x1080@120");
      //                               unset = preferred mode from EDID
      //   drm_compositor            : "auto" | "planes" | "gl"
      //   drm_modeset               : "auto" | "legacy" | "atomic"
      //   drm_allow_nonblock_modeset: "auto" | "yes" | "no"
      //   drm_primary_format        : "auto" | "xrgb8888" | "argb8888"
      //   drm_overlay_planes        : "auto" | "yes" | "no"
      //   drm_explicit_sync         : "auto" | "yes" | "no"
      //   drm_async_flip            : "auto" | "yes" | "no"
      //   drm_stage_cursor          : "auto" | "yes" | "no"
      //                               (auto = stage only on nvidia-drm)
      // FlutterView parses these into the DrmConfig enum fields. Anything
      // unrecognized is treated as "auto" with a warning.
      std::optional<std::string> drm_connector;
      std::optional<std::string> drm_mode;
      // DRM scanout rotation in degrees: 0 | 90 | 180 | 270. 90/270 swap the
      // Flutter viewport to the rotated extent and set the plane's rotation
      // property (the panel keeps its native mode). Default 0.
      std::optional<int> drm_rotation;
      // Backend selection when several backends are compiled into one binary.
      // Accepts a full registry key (wayland-egl | wayland-vulkan | drm-kms-egl
      // | drm-kms-vulkan | software | wayland-leased-drm-{egl,vulkan,software})
      // or, for back-compat, an "egl" | "vulkan" renderer-family hint. The bare
      // family name "wayland-leased-drm" is also accepted and resolves to the
      // first available leased tier. Empty/unset = the sole compiled backend.
      // Set per bundle under [view] in the bundle's TOML, or process-wide via
      // --backend. One process drives one backend today, so the first bundle's
      // value wins.
      std::optional<std::string> backend;

      // wayland-leased-drm knobs. All optional; see LeaseConfig in
      // backend/wayland_leased_drm/lease_client.h for the semantics.
      //   lease_device    : which wp_drm_lease_device_v1 to lease from when the
      //                     compositor advertises several (one per DRM node) —
      //                     a decimal index or a node path ("/dev/dri/card1").
      //                     Unset = the sole device; ambiguous + unset is
      //                     fatal.
      //   lease_connector : connector to request, by name ("HDMI-A-1"). Unset +
      //                     exactly one offer takes it; unset + several is
      //                     fatal and lists what was offered.
      //   lease_timeout_ms: wall-clock bound on the whole negotiation. Unset =
      //                     5000. Load-bearing: the spec lets a compositor
      //                     defer drm_fd until it regains DRM master, so
      //                     without a bound a VT-switched-away compositor would
      //                     hang startup.
      //   lease_on_revoke : what to do when the compositor revokes the lease —
      //                     which it does on every VT switch away from it, not
      //                     only on error.
      //                       "exit" (default) — log the cause and exit
      //                         non-zero, so a supervisor (systemd) restarts
      //                         and renegotiates from scratch. The honest
      //                         default until reacquire-in-place exists.
      //                       "gate"           — stop committing and keep
      //                         running with a frozen panel. Only useful for
      //                         debugging a revocation, since nothing recovers.
      //                       "reacquire"      — not implemented yet;
      //                         accepted and warned about, treated as "exit".
      //   lease_input     : whether the leased backend reads input from raw
      //                     evdev. A leased client has no wl_surface, so the
      //                     compositor delivers it no input — its only source
      //                     is libinput on /dev/input/event*, ungrabbed, which
      //                     on a host with a live Wayland session duplicates
      //                     every keystroke into both this process and the
      //                     session compositor.
      //                       "auto" (default) — read evdev on an embedded
      //                         target (no WAYLAND_DISPLAY), but disable it
      //                         when a host Wayland session is detected,
      //                         warning why.
      //                       "on"             — always read evdev, even under
      //                         a host session
      //                         (embedded-under-compositor).
      //                       "off"            — never read evdev.
      std::optional<std::string> lease_device;
      std::optional<std::string> lease_connector;
      std::optional<uint32_t> lease_timeout_ms;
      std::optional<std::string> lease_on_revoke;
      std::optional<std::string> lease_input;
      // Wayland compositor-protocol shell selection: "auto" (default) | "xdg" |
      // "agl" | "ivi" | "simple". The Display reads it from the first view.
      std::optional<std::string> shell;
      std::optional<std::string> drm_compositor;
      std::optional<std::string> drm_modeset;
      std::optional<std::string> drm_allow_nonblock_modeset;
      std::optional<std::string> drm_primary_format;
      std::optional<std::string> drm_overlay_planes;
      std::optional<std::string> drm_explicit_sync;
      std::optional<std::string> drm_async_flip;
      std::optional<std::string> drm_stage_cursor;
      // Bypass libseat entirely (--drm-no-seat / HOMESCREEN_DRM_NO_SEAT): the
      // DRM backend opens /dev/dri + /dev/input directly via group permissions
      // and becomes DRM master itself, also skipping the foreground-VT guard.
      // An opt-in escape hatch for headless / SSH / single-session kiosk
      // bring-up where no seat manager (logind/seatd) is running and the
      // operator guarantees nothing else holds the display. Gives up
      // VT-switch handling and session pause/resume. Unset = false.
      std::optional<bool> drm_no_seat;
      // Per-device relative-pointer transforms, repeatable:
      //   "<device-name-substring>=<0|90|180|270>[,flip-x][,flip-y]"
      // Rotates a built-in pointer's deltas to a rotated display (e.g. the
      // Steam Deck's right trackpad) while leaving an external mouse alone.
      // Matched by libinput device-name substring; first match wins.
      std::vector<std::string> drm_input_transforms;
      // Pin this view to a physical output by stable name (see
      // homescreen::ResolveOutput). Empty → binds to the primary output.
      // With an array [[view.output]], this is the FIRST entry (the primary
      // output / implicit view); the rest go in additional_outputs.
      homescreen::OutputMatch output;
      // extra outputs this ONE engine also drives (entries 2..N of a
      // [[view.output]] array). Empty for the common single-output view. Each
      // becomes an added Flutter view scanned out to its own connector on the
      // same card.
      std::vector<homescreen::OutputMatch> additional_outputs;
    } view;

    // Compositor debug HUD ([hud] table). Appearance + layout only; the HUD is
    // enabled by `enable` here or the IVI_HUD env var (no hot key). All ignored
    // unless the build has BUILD_HUD and the compositor is on.
    struct {
      std::optional<bool> enable;
      // Screen corner the overlay hugs: "top-left" (default), "top-right",
      // "bottom-left", "bottom-right".
      std::optional<std::string> corner;
      std::optional<double> margin;      // pixels from the corner
      std::optional<double> font_scale;  // multiplies the base font size
      std::optional<double> bg_alpha;    // window background opacity [0,1]
      // Text color as "#RRGGBB" or "#RRGGBBAA".
      std::optional<std::string> text_color;
    } hud;

    // Path to a master config.toml passed via --config. Process-level: it can
    // describe many [[view]] entries (each with its own bundle) and supplies
    // the process [global]/[sentry] tables. Empty/unset = legacy per-bundle
    // mode driven by -b. Declared last so existing structured bindings of
    // Config keep their member positions.
    std::optional<std::string> config_file;
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
   * @brief Apply the [global] (process-level) table onto a config
   * @param[in] root parsed TOML document root
   * @param[in,out] instance config
   * @return void
   * @relation
   * internal
   */
  static void get_global_parameters(toml::table* root, Config& instance);

  /**
   * @brief Apply a single [[view]] table onto a config
   * @param[in] view_tbl one [[view]] (or singular [view]) table
   * @param[in,out] instance config
   * @return void
   * @relation
   * internal
   */
  static void get_view_parameters(toml::table* view_tbl, Config& instance);

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
