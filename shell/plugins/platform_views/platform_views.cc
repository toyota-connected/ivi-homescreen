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

#include "platform_views.h"

#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "logging.h"
#include "platform_view.h"
#include "platform_views_registry.h"
#if defined(ENABLE_PLUGIN_FILAMENT)
#include "plugins/filament/filament.h"
#endif

void PlatformViews::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);

  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto method =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method_name = method->method_name();
  SPDLOG_DEBUG("[platform_views] {}", method_name);
  auto arguments = method->arguments();

  if (method_name == kMethodCreate && !arguments->IsNull()) {
    int32_t id = 0;
    std::string viewType;
    int32_t direction = 0;
    double width = 0;
    double height = 0;
    std::vector<uint8_t> params;

    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kKeyId && std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (key == kKeyViewType &&
                 std::holds_alternative<int32_t>(it.second)) {
        viewType.assign(std::get<std::string>(it.second));
      } else if (key == kKeyDirection &&
                 std::holds_alternative<int32_t>(it.second)) {
        direction = std::get<int32_t>(it.second);
      } else if (key == kKeyWidth &&
                 std::holds_alternative<double>(it.second)) {
        width = std::get<double>(it.second);
      } else if (key == kKeyHeight &&
                 std::holds_alternative<double>(it.second)) {
        height = std::get<double>(it.second);
      } else if (key == kKeyParams &&
                 std::holds_alternative<std::vector<uint8_t>>(it.second)) {
        params = std::get<std::vector<uint8_t>>(it.second);
      }
    }

#if defined(ENABLE_PLUGIN_FILAMENT)
    if (viewType == PlatformViewFilament::kPlatformViewType) {
      std::unique_ptr<PlatformView> platform_view =
          std::make_unique<PlatformViewFilament>(
              id, std::move(viewType), direction, width, height, params);
      PlatformViewsRegistry::GetRegistry().AddPlatformView(
          id, std::move(platform_view));
    } else
#endif
    {
      (void)direction;
      (void)width;
      (void)height;
      (void)params;
      result = codec.EncodeErrorEnvelope("unhandled_platform_view", viewType);
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
    }

    const auto res = flutter::EncodableValue(id);
    result = codec.EncodeSuccessEnvelope(&res);
  } else if (method_name == kMethodDispose) {
    Utils::PrintFlutterEncodableValue("arguments", *arguments);

    int32_t id = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      if (kKeyId == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      }
    }
    auto* view = PlatformViewsRegistry::GetInstance().GetPlatformView(id);
    view->~PlatformView();
    PlatformViewsRegistry::GetInstance().RemovePlatformView(id);
    result = codec.EncodeSuccessEnvelope();
  } else if (method_name == kMethodResize) {
    int32_t id = 0;
    double width = 0;
    double height = 0;

    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      if (kKeyId == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (kKeyWidth == std::get<std::string>(it.first) &&
                 std::holds_alternative<double>(it.second)) {
        width = std::get<double>(it.second);
      } else if (kKeyHeight == std::get<std::string>(it.first) &&
                 std::holds_alternative<double>(it.second)) {
        height = std::get<double>(it.second);
      }
    }

    PlatformViewsRegistry::GetInstance().GetPlatformView(id)->Resize(width,
                                                                     height);

    const auto res = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)},
        {flutter::EncodableValue("width"), flutter::EncodableValue(width)},
        {flutter::EncodableValue("height"), flutter::EncodableValue(height)},
    });
    result = codec.EncodeSuccessEnvelope(&res);
  } else if (method_name == kMethodSetDirection) {
    Utils::PrintFlutterEncodableValue("setDirection", *arguments);
    result = codec.EncodeSuccessEnvelope();
  } else if (method_name == kMethodClearFocus) {
    Utils::PrintFlutterEncodableValue("clearFocus", *arguments);
    result = codec.EncodeSuccessEnvelope();
  } else if (method_name == kMethodOffset) {
    Utils::PrintFlutterEncodableValue("offset", *arguments);
    result = codec.EncodeSuccessEnvelope();
  } else if (method_name == kMethodTouch && !arguments->IsNull()) {
    const auto args = std::get_if<flutter::EncodableList>(arguments);
    Utils::PrintFlutterEncodableList("[platform_views] touch:", *args);
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[PlatformViews] method {} is unhandled", method_name);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
