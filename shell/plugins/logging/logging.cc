/*
 * Copyright 2023 Toyota Connected North America
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

#include "logging.h"

#include "../../view/flutter_view.h"
#include "engine.h"

void LoggingPlugin::OnLogMessage(int level,
                                 const char* /* context */,
                                 const char* message) {
  switch (level) {
    case SPDLOG_LEVEL_TRACE:
      spdlog::trace(message);
      break;
    case SPDLOG_LEVEL_DEBUG:
      spdlog::debug(message);
      break;
    case SPDLOG_LEVEL_INFO:
      spdlog::info(message);
      break;
    case SPDLOG_LEVEL_WARN:
      spdlog::warn(message);
      break;
    case SPDLOG_LEVEL_ERROR:
      spdlog::error(message);
      break;
    case SPDLOG_LEVEL_CRITICAL:
      spdlog::critical(message);
      break;
    default:
    case SPDLOG_LEVEL_OFF:
      break;
  }
}

void LoggingPlugin::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  std::unique_ptr<std::vector<uint8_t>> result =
      codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  /* Get Logging Callback Function Pointer */
  if (method == kMethodGetLoggingCallbackFptr) {
    flutter::EncodableValue value(reinterpret_cast<int64_t>(&OnLogMessage));
    result = codec.EncodeSuccessEnvelope(&value);
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
