/*
 * Copyright 2024 Toyota Connected North America
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

#undef _HAS_EXCEPTIONS

#include "messages.h"

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>

#include <optional>
#include <string>

#include "plugins/common/common.h"

namespace google_sign_in_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

// Method Constants
static constexpr const char* kMethodInit = "init";
static constexpr const char* kMethodSignIn = "signIn";
static constexpr const char* kMethodSignInSilently = "signInSilently";
static constexpr const char* kMethodGetTokens = "getTokens";
static constexpr const char* kMethodSignOut = "signOut";
static constexpr const char* kMethodDisconnect = "disconnect";

// Method Argument Constants
static constexpr const char* kMethodArgSignInOption = "signInOption";
static constexpr const char* kMethodArgScopes = "scopes";
static constexpr const char* kMethodArgHostedDomain = "hostedDomain";
static constexpr const char* kMethodArgClientId = "clientId";
static constexpr const char* kMethodArgServerClientId = "serverClientId";
static constexpr const char* kMethodArgForceCodeForRefreshToken =
    "forceCodeForRefreshToken";
static constexpr const char* kMethodArgShouldRecoverAuth = "shouldRecoverAuth";

static constexpr const char* kMethodResponseKeyEmail = "email";

/// The codec used by GoogleSignInApi.
const flutter::StandardMethodCodec& GoogleSignInApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `GoogleSignInApi` to handle messages through the
// `binary_messenger`.
void GoogleSignInApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                            GoogleSignInApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<>>(
        binary_messenger, "plugins.flutter.io/google_sign_in", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<EncodableValue>& call,
                std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
            const auto& method = call.method_name();

            if (method == kMethodInit) {
              SPDLOG_DEBUG("[google_sign_in] <init>");

              const auto args = std::get_if<EncodableMap>(*call.arguments());
              if (args->IsNull()) {
                result->Error("invalid_arguments", "");
                return;
              }

              std::string signInOption;
              std::vector<std::string> requestedScopes;
              std::string hostedDomain;
              std::string clientId;
              std::string serverClientId;
              bool forceCodeForRefreshToken{};

              for (auto& it : *args) {
                const auto key = std::get<std::string>(it.first);
                if (key == kMethodArgSignInOption && !it.second.IsNull()) {
                  signInOption.assign(std::get<std::string>(it.second));
                } else if (key == kMethodArgScopes && !it.second.IsNull()) {
                  auto requestedScopes_ =
                      std::get<std::vector<EncodableValue>>(it.second);
                  for (auto& scope : requestedScopes_) {
                    if (!scope.IsNull()) {
                      auto val = std::get<std::string>(scope);
                      requestedScopes.push_back(std::move(val));
                    }
                  }
                } else if (key == kMethodArgHostedDomain &&
                           !it.second.IsNull()) {
                  hostedDomain.assign(std::get<std::string>(it.second));
                } else if (key == kMethodArgClientId && !it.second.IsNull()) {
                  clientId.assign(std::get<std::string>(it.second));
                } else if (key == kMethodArgServerClientId &&
                           !it.second.IsNull()) {
                  serverClientId.assign(std::get<std::string>(it.second));
                } else if (key == kMethodArgForceCodeForRefreshToken &&
                           !it.second.IsNull()) {
                  forceCodeForRefreshToken = std::get<bool>(it.second);
                }
              }

              api->Init(requestedScopes, std::move(hostedDomain),
                        std::move(signInOption), std::move(clientId),
                        std::move(serverClientId), forceCodeForRefreshToken);
              result->Success();
            } else if (method == kMethodSignIn) {
              SPDLOG_DEBUG("[google_sign_in] <signIn>");
              result->Success(api->GetUserData());
            } else if (method == kMethodSignInSilently) {
              SPDLOG_DEBUG("[google_sign_in] <signInSilently>");
              result->Success(api->GetUserData());
            } else if (method == kMethodGetTokens) {
              SPDLOG_DEBUG("[google_sign_in] <getTokens>");
              const auto args = std::get_if<EncodableMap>(*call.arguments());
              if (args->IsNull()) {
                result->Error("invalid_arguments", "");
                return;
              }
              std::string email;
              bool shouldRecoverAuth{};
              for (auto& it : *args) {
                const auto key = std::get<std::string>(it.first);
                if (key == kMethodResponseKeyEmail && !it.second.IsNull()) {
                  email.assign(std::get<std::string>(it.second));
                } else if (key == kMethodArgShouldRecoverAuth &&
                           !it.second.IsNull()) {
                  shouldRecoverAuth = std::get<bool>(it.second);
                }
              }
              SPDLOG_DEBUG("\temail: [{}]", email);
              SPDLOG_DEBUG("\tshouldRecoverAuth: {}", shouldRecoverAuth);
              result->Success(api->GetTokens(email, shouldRecoverAuth));
            } else if (method == kMethodSignOut) {
              SPDLOG_DEBUG("[google_sign_in] <signOut>");
              result->Success(api->GetUserData());
            } else if (method == kMethodDisconnect) {
              SPDLOG_DEBUG("[google_sign_in] <disconnect>");
              result->Success(api->GetUserData());
            }

            return result->Success();
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace google_sign_in_plugin