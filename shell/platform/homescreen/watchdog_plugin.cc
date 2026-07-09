/*
 * Copyright 2026 Toyota Connected North America
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

#include "shell/platform/homescreen/watchdog_plugin.h"

#include <flutter/standard_method_codec.h>

#include "logging/logging.h"

// StandardMethodCodec encodes small Dart integers as int32_t, not int64_t.
// This helper safely widens either representation to int64_t.
static int64_t GetEncodableInt(const flutter::EncodableValue& val) {
  if (const auto* i32 = std::get_if<int32_t>(&val)) {
    return static_cast<int64_t>(*i32);
  }
  return std::get<int64_t>(val);
}

WatchdogPlugin::WatchdogPlugin(flutter::BinaryMessenger* messenger)
    : channel_(std::make_unique<flutter::MethodChannel<>>(
          messenger,
          kChannelName,
          &flutter::StandardMethodCodec::GetInstance())) {
  channel_->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
             result) { HandleMethodCall(call, std::move(result)); });
}

bool WatchdogPlugin::checkWatchdogSource(const int64_t source) {
  return source >= 0;
}

void WatchdogPlugin::externalStart(int64_t source) {
#if BUILD_WATCHDOG
  if (!checkWatchdogSource(source)) {
    spdlog::error("WatchdogPlugin: externalStart: Invalid source {}", source);
    return;
  }
  Watchdog::getInstance().start(static_cast<WatchdogSource>(source));
#else
  (void)source;
#endif
}

void WatchdogPlugin::externalPet(int64_t source) {
#if BUILD_WATCHDOG
  if (!checkWatchdogSource(source)) {
    spdlog::error("WatchdogPlugin: externalPet: Invalid source {}", source);
    return;
  }
  Watchdog::getInstance().pet(static_cast<WatchdogSource>(source));
#else
  (void)source;
#endif
}

void WatchdogPlugin::externalStop(int64_t source) {
#if BUILD_WATCHDOG
  if (!checkWatchdogSource(source)) {
    spdlog::error("WatchdogPlugin: externalStop: Invalid source {}", source);
    return;
  }
  Watchdog::getInstance().stop(static_cast<WatchdogSource>(source));
#else
  (void)source;
#endif
}

void WatchdogPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string& method = method_call.method_name();

  if (method == kMethodGetCallbacks) {
    flutter::EncodableMap map;
    map.emplace(
        flutter::EncodableValue("start"),
        flutter::EncodableValue(reinterpret_cast<int64_t>(&externalStart)));
    map.emplace(
        flutter::EncodableValue("pet"),
        flutter::EncodableValue(reinterpret_cast<int64_t>(&externalPet)));
    map.emplace(
        flutter::EncodableValue("stop"),
        flutter::EncodableValue(reinterpret_cast<int64_t>(&externalStop)));
    result->Success(flutter::EncodableValue(std::move(map)));
  } else if (method == kMethodStart) {
#if BUILD_WATCHDOG
    int64_t source = 0;
    if (const auto* args =
            std::get_if<flutter::EncodableMap>(method_call.arguments())) {
      if (const auto it = args->find(flutter::EncodableValue("source"));
          it != args->end()) {
        source = GetEncodableInt(it->second);
      }
    }
    if (!checkWatchdogSource(source)) {
      spdlog::error("WatchdogPlugin: start: Invalid source {}", source);
      result->Error("invalid_source", "Source out of range");
      return;
    }
    Watchdog::getInstance().start(static_cast<WatchdogSource>(source));
#endif
    result->Success();
  } else if (method == kMethodPet) {
#if BUILD_WATCHDOG
    int64_t source = 0;
    if (const auto* args =
            std::get_if<flutter::EncodableMap>(method_call.arguments())) {
      if (const auto it = args->find(flutter::EncodableValue("source"));
          it != args->end()) {
        source = GetEncodableInt(it->second);
      }
    }
    if (!checkWatchdogSource(source)) {
      spdlog::error("WatchdogPlugin: pet: Invalid source {}", source);
      result->Error("invalid_source", "Source out of range");
      return;
    }
    Watchdog::getInstance().pet(static_cast<WatchdogSource>(source));
#endif
    result->Success();
  } else if (method == kMethodStop) {
#if BUILD_WATCHDOG
    int64_t source = 0;
    if (const auto* args =
            std::get_if<flutter::EncodableMap>(method_call.arguments())) {
      if (const auto it = args->find(flutter::EncodableValue("source"));
          it != args->end()) {
        source = GetEncodableInt(it->second);
      }
    }
    if (!checkWatchdogSource(source)) {
      spdlog::error("WatchdogPlugin: stop: Invalid source {}", source);
      result->Error("invalid_source", "Source out of range");
      return;
    }
    Watchdog::getInstance().stop(static_cast<WatchdogSource>(source));
#endif
    result->Success();
  } else {
    result->NotImplemented();
  }
}
