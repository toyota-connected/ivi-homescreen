/*
 * Copyright 2020-2024 Toyota Connected North America
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

#include "logging_handler.h"

#include <flutter/standard_method_codec.h>

#include "logging/logging.h"

LoggingHandler::LoggingHandler(flutter::BinaryMessenger* messenger,
                               FlutterView* /* view */)
    : channel_(std::make_unique<flutter::MethodChannel<>>(
          messenger,
          "logging",
          &flutter::StandardMethodCodec::GetInstance())) {
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) { HandleMethodCall(call, std::move(result)); });
}

void LoggingHandler::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
    const {
  const std::string& method = method_call.method_name();

  if (method == "get_logging_callback_fptr") {
    const flutter::EncodableValue value(
        reinterpret_cast<int64_t>(&LoggingHandler::OnLogMessage));
    result->Success(flutter::EncodableValue(value));
  } else {
    result->NotImplemented();
  }
}

void LoggingHandler::OnLogMessage(int level,
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
