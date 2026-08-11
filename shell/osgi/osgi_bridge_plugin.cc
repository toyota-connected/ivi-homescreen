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

#include "osgi_bridge_plugin.h"

#include <flutter/standard_method_codec.h>

#include <string>

#include "bridge_registry.h"
#include "logging/logging.h"

namespace ihs::osgi {

namespace {

// StandardMethodCodec narrows small Dart ints to int32_t, so a port or an
// address may arrive as either width. Mirrors the helper in watchdog_plugin.cc.
bool ReadInt64(const flutter::EncodableValue& value, int64_t& out) {
  if (const auto* i32 = std::get_if<int32_t>(&value)) {
    out = *i32;
    return true;
  }
  if (const auto* i64 = std::get_if<int64_t>(&value)) {
    out = *i64;
    return true;
  }
  return false;
}

bool ReadString(const flutter::EncodableValue& value, std::string& out) {
  if (const auto* s = std::get_if<std::string>(&value)) {
    out = *s;
    return true;
  }
  return false;
}

// Look up @key in @map. Returns nullptr when absent, so callers can tell
// "missing" from "present but wrong type".
const flutter::EncodableValue* Find(const flutter::EncodableMap& map,
                                    const char* key) {
  const auto it = map.find(flutter::EncodableValue(std::string(key)));
  return it == map.end() ? nullptr : &it->second;
}

}  // namespace

OsgiBridgePlugin::OsgiBridgePlugin(flutter::BinaryMessenger* messenger)
    : channel_(std::make_unique<flutter::MethodChannel<>>(
          messenger,
          kChannelName,
          &flutter::StandardMethodCodec::GetInstance())) {
  channel_->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
             result) { HandleMethodCall(call, std::move(result)); });
}

void OsgiBridgePlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto* args =
      std::get_if<flutter::EncodableMap>(method_call.arguments());
  if (args == nullptr) {
    result->Error(kErrorBadArgs, "expected a map of arguments");
    return;
  }

  auto& registry = BridgeRegistry::Instance();

  if (method_call.method_name() == kMethodInit) {
    std::string role;
    if (const auto* v = Find(*args, "role");
        v == nullptr || !ReadString(*v, role)) {
      result->Error(kErrorBadArgs, "'role' must be a string");
      return;
    }

    int64_t dl_data = 0;
    if (const auto* v = Find(*args, "dl_data");
        v == nullptr || !ReadInt64(*v, dl_data)) {
      result->Error(kErrorBadArgs, "'dl_data' must be an int");
      return;
    }

    int64_t port = 0;
    if (const auto* v = Find(*args, "port");
        v == nullptr || !ReadInt64(*v, port)) {
      result->Error(kErrorBadArgs, "'port' must be an int");
      return;
    }

    // Every caller supplies the DL data because any isolate may be the first to
    // arrive; the registry makes all but the first a no-op.
    if (!registry.InitializeDartApi(dl_data)) {
      result->Error(kErrorDartApi,
                    "Dart_InitializeApiDL failed; the vendored Dart DL headers "
                    "do not match the running VM");
      return;
    }

    if (role == "framework") {
      const std::optional<size_t> served = registry.SetFrameworkPort(port);
      if (!served.has_value()) {
        result->Error(kErrorRejected, "framework port rejected");
        return;
      }
      ihs::log::debug(
          "[osgi] bridge: framework registered; served {} waiting bundle(s)",
          *served);
      result->Success(flutter::EncodableValue(true));
      return;
    }

    if (role == "bundle") {
      std::string symbolic_name;
      if (const auto* v = Find(*args, "symbolic_name");
          v == nullptr || !ReadString(*v, symbolic_name)) {
        result->Error(kErrorBadArgs,
                      "'symbolic_name' must be a string for role=bundle");
        return;
      }
      if (!registry.RegisterBundle(symbolic_name, port)) {
        result->Error(kErrorRejected, "bundle registration rejected");
        return;
      }
      // Not an error: the framework may legitimately still be starting. The
      // port is delivered when it registers, so the bundle simply waits on its
      // receive port either way.
      ihs::log::debug(
          "[osgi] bridge: bundle '{}' registered ({})", symbolic_name,
          registry.framework_port().has_value() ? "framework port delivered"
                                                : "awaiting framework");
      result->Success(flutter::EncodableValue(true));
      return;
    }

    result->Error(kErrorBadArgs, "'role' must be 'framework' or 'bundle'");
    return;
  }

  if (method_call.method_name() == kMethodActive ||
      method_call.method_name() == kMethodStopped) {
    std::string symbolic_name;
    if (const auto* v = Find(*args, "symbolic_name");
        v == nullptr || !ReadString(*v, symbolic_name)) {
      result->Error(kErrorBadArgs, "'symbolic_name' must be a string");
      return;
    }
    const bool is_active = method_call.method_name() == kMethodActive;
    const bool ok = is_active ? registry.ReportActive(symbolic_name)
                              : registry.ReportStopped(symbolic_name);
    if (!ok) {
      result->Error(kErrorRejected, "bundle is not registered");
      return;
    }
    ihs::log::debug("[osgi] bridge: bundle '{}' reported {}", symbolic_name,
                    is_active ? "ACTIVE" : "STOPPED");
    result->Success(flutter::EncodableValue(true));
    return;
  }

  if (method_call.method_name() == kMethodShutdown) {
    std::string symbolic_name;
    if (const auto* v = Find(*args, "symbolic_name");
        v == nullptr || !ReadString(*v, symbolic_name)) {
      result->Error(kErrorBadArgs, "'symbolic_name' must be a string");
      return;
    }
    result->Success(
        flutter::EncodableValue(registry.UnregisterBundle(symbolic_name)));
    return;
  }

  result->NotImplemented();
}

}  // namespace ihs::osgi
