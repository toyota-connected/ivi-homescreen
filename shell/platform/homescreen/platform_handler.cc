// Copyright 2020 Toyota Connected North America
// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include "platform_handler.h"

#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"

static constexpr char kNoWindowError[] = "Missing window error";
static constexpr char kUnknownClipboardFormatError[] =
    "Unknown clipboard format error";

PlatformHandler::PlatformHandler(flutter::BinaryMessenger* messenger,
                                 FlutterView* view)
    : channel_(std::make_unique<flutter::MethodChannel<rapidjson::Document>>(
          messenger,
          "flutter/platform",
          &flutter::JsonMethodCodec::GetInstance())),
      view_(view) {
  channel_->SetMethodCallHandler(
      [this](
          const flutter::MethodCall<rapidjson::Document>& call,
          std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
}

void PlatformHandler::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) const {
  const std::string& method = method_call.method_name();

  if (method == "Clipboard.getData") {
    if (!view_) {
      result->Error(kNoWindowError,
                    "Clipboard is not available in headless mode.");
      return;
    }
    // Only one string argument is expected.
    const rapidjson::Value& format = method_call.arguments()[0];

    if (strcmp(format.GetString(), "text/plain") != 0) {
      result->Error(kUnknownClipboardFormatError,
                    "Clipboard API only supports text.");
      return;
    }

    const char* clipboardData = "";  // TODO
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
    document.AddMember(rapidjson::Value("text", allocator),
                       rapidjson::Value(clipboardData, allocator), allocator);
    result->Success(document);
  } else if (method == "Clipboard.setData") {
    if (!view_) {
      result->Error(kNoWindowError,
                    "Clipboard is not available in headless mode.");
      return;
    }
    const rapidjson::Value& document = *method_call.arguments();
    const rapidjson::Value::ConstMemberIterator itr =
        document.FindMember("text");
    if (itr == document.MemberEnd()) {
      result->Error(kUnknownClipboardFormatError,
                    "Missing text to store on clipboard.");
      return;
    }
    result->Success();
  } else {
    result->NotImplemented();
  }
}
