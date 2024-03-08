#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "flutter_desktop_engine_state.h"
#include "flutter_homescreen.h"
#include "nav_render_error.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

namespace nav_render_view_plugin {
class NavRenderTexture : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  NavRenderTexture(flutter::PluginRegistrar* registrar);

  ~NavRenderTexture() override;

  ErrorOr<flutter::EncodableMap> Create(const std::string& access_token,
                                        bool map_flutter_assets,
                                        const std::string& asset_path,
                                        const std::string& cache_folder,
                                        const std::string& misc_folder,
                                        int interface_version);

  // Disallow copy and assign.
  NavRenderTexture(const NavRenderTexture&) = delete;
  NavRenderTexture& operator=(const NavRenderTexture&) = delete;
 private:
  std::unique_ptr<flutter::MethodChannel<>> channel_;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

};
}  // namespace nav_render_view_plugin
