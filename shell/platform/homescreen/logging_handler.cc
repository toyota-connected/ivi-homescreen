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
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
             result) { HandleMethodCall(call, std::move(result)); });
}

void LoggingHandler::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string& method = method_call.method_name();

  if (method == "get_logging_callback_fptr") {
    const flutter::EncodableValue value(
        reinterpret_cast<int64_t>(&LoggingHandler::OnLogMessage));
    result->Success(flutter::EncodableValue(value));
  } else {
    result->NotImplemented();
  }
}

namespace {
// Level values passed by the Dart logging callback follow the historical
// spdlog numeric scale (trace=0, debug=1, info=2, warn=3, error=4, critical=5,
// off=6) — distinct from the ihs IhsLogLevel scale, so map explicitly.
enum DartLogLevel {
  kLevelTrace = 0,
  kLevelDebug = 1,
  kLevelInfo = 2,
  kLevelWarn = 3,
  kLevelError = 4,
  kLevelCritical = 5,
  kLevelOff = 6,
};

// A single ihs log record is capped at IHS_LOG_TEXT_CAPACITY bytes, but Dart
// messages — notably exception stack traces — routinely exceed that. Emit the
// message in chunks so nothing is silently truncated: break on newlines first
// (natural per stack frame), then hard-split any remaining over-length run.
template <class Emit>
void emit_chunked(const char* message, Emit emit) {
  if (message == nullptr) {
    return;
  }
  constexpr std::size_t kMax = IHS_LOG_TEXT_CAPACITY - 1;
  std::string_view rest{message};
  if (rest.empty()) {
    emit(rest);
    return;
  }
  while (!rest.empty()) {
    std::size_t take = rest.size();
    const std::size_t nl = rest.find('\n');
    const bool at_newline = nl != std::string_view::npos && nl < take;
    if (at_newline) {
      take = nl;
    }
    if (take > kMax) {
      take = kMax;
      emit(rest.substr(0, take));
      rest.remove_prefix(take);
      continue;
    }
    emit(rest.substr(0, take));
    rest.remove_prefix(at_newline ? take + 1 : take);  // consume the '\n' too
  }
}
}  // namespace

void LoggingHandler::OnLogMessage(int level,
                                  const char* /* context */,
                                  const char* message) {
  if (level == kLevelOff) {
    return;
  }
  const auto emit = [level](std::string_view line) {
    switch (level) {
      case kLevelTrace:
        ihs::log::trace(line);
        break;
      case kLevelDebug:
        ihs::log::debug(line);
        break;
      case kLevelInfo:
        ihs::log::info(line);
        break;
      case kLevelWarn:
        ihs::log::warn(line);
        break;
      case kLevelError:
        ihs::log::error(line);
        break;
      case kLevelCritical:
        ihs::log::critical(line);
        break;
      default:
        break;
    }
  };
  emit_chunked(message, emit);
}
