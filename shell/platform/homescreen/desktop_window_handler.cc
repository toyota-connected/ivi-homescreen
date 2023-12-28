
#include "platform/homescreen/desktop_window_handler.h"

#include <flutter/standard_method_codec.h>
#include "utils.h"

namespace flutter {
DesktopWindowHandler::DesktopWindowHandler(
    flutter::BinaryMessenger* messenger,
    FlutterView* /* view */) {
  auto channel = std::make_unique<flutter::MethodChannel<EncodableValue>>(
      messenger, "desktop_window",
      &flutter::StandardMethodCodec::GetInstance());
  channel->SetMethodCallHandler(
      [&](const MethodCall<EncodableValue>& methodCall,
          std::unique_ptr<MethodResult<EncodableValue>> result) {
        if (methodCall.method_name() == "setMinWindowSize") {
          const auto& args = std::get_if<EncodableMap>(methodCall.arguments());

          double width = 0;
          double height = 0;
          for (auto& it : *args) {
            if ("width" == std::get<std::string>(it.first) &&
                       std::holds_alternative<double>(it.second)) {
              width = std::get<double>(it.second);
            } else if ("height" == std::get<std::string>(it.first) &&
                       std::holds_alternative<double>(it.second)) {
              height = std::get<double>(it.second);
            }
          }
          spdlog::debug("[desktop_window] setMinWindowSize: width: {}, height: {}", width, height);
          //TODO set compositor surface size to not be less than specified width x height
          result->Success();
        } else {
          result->NotImplemented();
        }
      });
}
}  // namespace flutter