
#include "nav_render_texture.h"

#include "libnav_render.h"

namespace nav_render_view_plugin {

static constexpr int kExpectedRenderApiVersion = 0x00010002;

void NavRenderTexture::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  if (!LibNavRender::IsPresent()) {
    spdlog::error("[NavRenderViewPlugin] libnav_render.so missing");
  }
  if (LibNavRender::kExpectedTextureApiVersion != LibNavRender->TextureGetInterfaceVersion()) {
    spdlog::error("[NavRenderViewPlugin] unexpected interface version: {}",
                  LibNavRender->TextureGetInterfaceVersion());
  }

  auto plugin = std::make_unique<NavRenderTexture>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

NavRenderTexture::NavRenderTexture(flutter::PluginRegistrar* registrar) {
  channel_ = std::make_unique<flutter::MethodChannel<>>(
      registrar->messenger(), "nav_render_view", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) { HandleMethodCall(call, std::move(result)); });
}

void NavRenderTexture::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (call.method_name() == "create") {
    std::string access_token;
    std::string module;
    bool map_flutter_assets{};
    std::string asset_path;
    std::string cache_folder;
    std::string misc_folder;
    int interface_version = 0;

    const auto& args = std::get_if<flutter::EncodableMap>(call.arguments());

    for (const auto& it : *args) {
      const auto& key = std::get<std::string>(it.first);
      if (key == "access_token") {
        if (std::holds_alternative<std::string>(it.second)) {
          access_token = std::get<std::string>(it.second);
        }
      } else if (key == "map_flutter_assets") {
        if (std::holds_alternative<bool>(it.second)) {
          map_flutter_assets = std::get<bool>(it.second);
        }
      } else if (key == "asset_path") {
        if (std::holds_alternative<std::string>(it.second)) {
          asset_path = std::get<std::string>(it.second);
        }
      } else if (key == "cache_folder") {
        if (std::holds_alternative<std::string>(it.second)) {
          cache_folder = std::get<std::string>(it.second);
        }
      } else if (key == "misc_folder") {
        if (std::holds_alternative<std::string>(it.second)) {
          misc_folder = std::get<std::string>(it.second);
        }
      } else if (key == "intf_ver") {
        if (std::holds_alternative<int>(it.second)) {
          interface_version = std::get<int>(it.second);
        }
      }
    }
    auto res = Create(access_token, map_flutter_assets, asset_path,
                      cache_folder, misc_folder, interface_version);
    if (res.has_error()) {
      result->Error(res.error().message(), res.error().code(),
                    res.error().details());
    } else {
      result->Success(flutter::EncodableValue(res.value()));
    }
  }
  else {
    result->NotImplemented();
  }
}

NavRenderTexture::~NavRenderTexture() = default;

ErrorOr<flutter::EncodableMap> NavRenderTexture::Create(
    const std::string& access_token,
    bool map_flutter_assets,
    const std::string& asset_path,
    const std::string& cache_folder,
    const std::string& misc_folder,
    int interface_version) {
  NavRenderConfig config{};
  config.dpy = nullptr;
  config.context = nullptr;
  config.framebufferId = 0;
  config.access_token = access_token.c_str();
  config.width = 640;
  config.height = 480;
  if (map_flutter_assets) {
    config.asset_path = asset_path.c_str();
  } else {
    config.asset_path = asset_path.c_str();
  }
  config.cache_folder = cache_folder.c_str();
  config.misc_folder = misc_folder.c_str();
  config.pfn_log = nullptr;
  config.pfn_gl_loader = nullptr;
  config.native_window = nullptr;

  auto ctx = LibNavRender->TextureInitialize2(&config);

  return flutter::EncodableMap{};
}

}  // namespace nav_render_view_plugin
