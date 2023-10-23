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

#include "google_sign_in.h"

#include "engine.h"
#include "json_utils.h"
#include "logging.h"

rapidjson::Document GoogleSignIn::GetClientSecret() {
  std::string path;
  auto env_var = getenv(kClientSecretPathEnvironmentVariable);
  if (env_var) {
    path.assign(env_var);
  }
  return JsonUtils::GetJsonDocumentFromFile(path);
}

rapidjson::Document GoogleSignIn::GetClientCredentials() {
  std::string path;
  auto env_var = getenv(kClientCredentialsPathEnvironmentVariable);
  if (env_var) {
    path.assign(env_var);
  }
  return JsonUtils::GetJsonDocumentFromFile(path);
}

bool GoogleSignIn::UpdateClientCredentialFile(
    rapidjson::Document& client_credential_doc) {
  auto env_var = getenv(kClientCredentialsPathEnvironmentVariable);
  std::string path;
  if (env_var) {
    path.assign(env_var);
    if (path.empty()) {
      spdlog::error("Missing File Path: {}", path);
      return false;
    }

    std::filesystem::path p(path);
    if (!std::filesystem::exists(path)) {
      std::filesystem::create_directories(p.parent_path());
    } else {
      std::filesystem::remove(path);
    }
  }

  return JsonUtils::WriteJsonDocumentToFile(path, client_credential_doc);
}

rapidjson::Document GoogleSignIn::SwapAuthCodeForToken(
    rapidjson::Document& client_secret_doc,
    rapidjson::Document& client_credential_doc) {
  rapidjson::Document doc;
  auto secret_obj = client_secret_doc.GetObject();
  auto installed_secret_obj = secret_obj[kKeyInstalled].GetObject();
  auto creds_obj = client_credential_doc.GetObject();

  std::string auth_code = creds_obj[kKeyAuthCode].GetString();
  std::string token_uri = installed_secret_obj[kKeyTokenUri].GetString();
  std::string client_id = installed_secret_obj[kKeyClientId].GetString();
  std::string client_secret =
      installed_secret_obj[kKeyClientSecret].GetString();

  if (auth_code.empty() || token_uri.empty() || client_id.empty() ||
      client_secret.empty()) {
    doc.Parse("{}");
    return std::move(doc);
  }

  CurlClient client;
  std::vector<std::string> headers{};
  std::vector<std::pair<std::string, std::string>> url_form;
  url_form.emplace_back(std::move(std::make_pair(kKeyCode, auth_code)));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyClientId, std::move(client_id))));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyClientSecret, std::move(client_secret))));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyRedirectUri, kValueRedirectUri)));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyGrantType, kValueAuthorizationCode)));

  client.Init(token_uri, headers, url_form);
  auto response = client.RetrieveContent();

  if (client.GetCode() != CURLE_OK) {
    doc.Parse("{}");
    return std::move(doc);
  }

  doc.Parse(response.c_str());
  if (doc.GetParseError() != rapidjson::kParseErrorNone) {
    doc.Parse("{}");
    return std::move(doc);
  }

  auto resp = doc.GetObject();
  auto& allocator = doc.GetAllocator();

  // change expires_in to expires_at
  auto expires_in = resp["expires_in"].GetInt64();
  resp.RemoveMember("expires_in");
  int64_t expires_at = Utils::GetEpochTimeInSeconds() + expires_in;
  resp.AddMember("expires_at", expires_at, allocator);

  // preserve auth_code
  resp.AddMember(rapidjson::Value(kKeyAuthCode, allocator).Move(),
                 rapidjson::Value(auth_code.c_str(), allocator).Move(),
                 allocator);

  if (!UpdateClientCredentialFile(doc)) {
    doc.Parse("{}");
    return std::move(doc);
  }

  return std::move(doc);
}

rapidjson::Document GoogleSignIn::RefreshToken(
    rapidjson::Document& client_secret_doc,
    rapidjson::Document& client_credential_doc) {
  rapidjson::Document doc;
  auto secret_obj = client_secret_doc.GetObject();
  auto installed_secret_obj = secret_obj[kKeyInstalled].GetObject();
  auto creds_obj = client_credential_doc.GetObject();

  std::string refresh_token = creds_obj[kKeyRefreshToken].GetString();
  std::string auth_code = creds_obj[kKeyAuthCode].GetString();
  int64_t expires_at = creds_obj[kKeyExpiresAt].GetInt64();

  // Has token expired?
  auto now = Utils::GetEpochTimeInSeconds();
  spdlog::trace("[google_sign_in] Now: {}", now);
  spdlog::trace("[google_sign_in] Token Expires At: {}", expires_at);
  if (expires_at > now) {
    spdlog::debug("[Google Sign In] Token Expires In {} Seconds",
                  now - expires_at);
    return std::move(client_credential_doc);
  }
  SPDLOG_DEBUG("[Google Sign In] Refreshing Token");

  std::string token_uri = installed_secret_obj[kKeyTokenUri].GetString();
  std::string client_id = installed_secret_obj[kKeyClientId].GetString();
  std::string client_secret =
      installed_secret_obj[kKeyClientSecret].GetString();

  if (token_uri.empty() || client_id.empty() || client_secret.empty() ||
      refresh_token.empty() || auth_code.empty()) {
    doc.Parse("{}");
    return std::move(doc);
  }

  CurlClient client;
  std::vector<std::string> headers{};
  std::vector<std::pair<std::string, std::string>> url_form;
  url_form.emplace_back(
      std::move(std::make_pair(kKeyRefreshToken, refresh_token)));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyClientId, std::move(client_id))));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyClientSecret, std::move(client_secret))));
  url_form.emplace_back(
      std::move(std::make_pair(kKeyGrantType, kValueRefreshToken)));

  client.Init(token_uri, headers, url_form);
  auto response = client.RetrieveContent();

  if (client.GetCode() != CURLE_OK) {
    doc.Parse("{}");
    return std::move(doc);
  }

  doc.Parse(response.c_str());
  if (doc.GetParseError() != rapidjson::kParseErrorNone) {
    spdlog::error("[google_sign_in] Failure Parsing Refresh Token Response: {}",
                  static_cast<int>(doc.GetParseError()));
    doc.Parse("{}");
    return std::move(doc);
  }

  auto resp = doc.GetObject();
  auto& allocator = doc.GetAllocator();

  if (resp.HasMember("error") && resp["error"].IsString() &&
      resp.HasMember("error_description") &&
      resp["error_description"].IsString()) {
    spdlog::error("[google_sign_in] Refresh Token Error: {} - {}",
                  resp["error"].GetString(),
                  resp["error_description"].GetString());
    return std::move(doc);
  }

  // change expires_in to expires_at
  auto expires_in = resp[kKeyExpiresIn].GetInt64();
  resp.RemoveMember(kKeyExpiresIn);
  expires_at = Utils::GetEpochTimeInSeconds() + expires_in;
  resp.AddMember("expires_at", expires_at, allocator);

  // preserve refresh_token
  resp.AddMember(rapidjson::Value(kKeyRefreshToken, allocator).Move(),
                 rapidjson::Value(refresh_token.c_str(), allocator).Move(),
                 allocator);

  // preserve auth_code
  resp.AddMember(rapidjson::Value(kKeyAuthCode, allocator).Move(),
                 rapidjson::Value(auth_code.c_str(), allocator).Move(),
                 allocator);

  if (!UpdateClientCredentialFile(doc)) {
    doc.Parse("{}");
    return std::move(doc);
  }

  return std::move(doc);
}

bool GoogleSignIn::CreateDefaultClientCredentialFile() {
  auto env_var = getenv(kClientCredentialsPathEnvironmentVariable);
  std::string path;
  if (env_var) {
    path.assign(env_var);
    if (path.empty()) {
      spdlog::error("Missing File Path: {}", path);
      return false;
    }

    std::filesystem::path p(path);
    if (!std::filesystem::exists(path)) {
      std::filesystem::create_directories(p.parent_path());
    } else {
      std::filesystem::remove(path);
    }
  }

  return JsonUtils::AddEmptyKeyToFile(path, kKeyAuthCode);
}

std::string GoogleSignIn::GetAuthUrl(rapidjson::Document& secret_doc,
                                     const std::vector<std::string>& scopes) {
  std::string res;
  auto secret_obj = secret_doc.GetObject();
  std::string client_id_, auth_uri_;
  if (secret_obj.HasMember(kKeyInstalled)) {
    auto obj = secret_obj[kKeyInstalled].GetObject();
    if (!obj.HasMember(kKeyClientId) || !obj.HasMember(kKeyAuthUri) ||
        !obj[kKeyClientId].IsString() || !obj[kKeyAuthUri].IsString()) {
      spdlog::error("Invalid client_secret object");
      return res;
    } else {
      client_id_ = obj[kKeyClientId].GetString();
      auth_uri_ = obj[kKeyAuthUri].GetString();
      std::stringstream ss;
      ss << auth_uri_ << "?client_id=" << client_id_;
      ss << "&redirect_uri=urn:ietf:wg:oauth:2.0:oob";
      ss << "&scope=";
      for (auto& scope : scopes) {
        ss << scope << "%20";
      }
      ss << "https://www.googleapis.com/auth/userinfo.profile";
      ss << "&response_type=code";
      res.assign(ss.str());
    }
  }
  return res;
}

bool GoogleSignIn::AuthCodeValuePresent(rapidjson::Document& credentials_doc) {
  auto obj = credentials_doc.GetObject();
  if (obj.HasMember(kKeyAuthCode) && obj[kKeyAuthCode].IsString()) {
    std::string auth_code = obj[kKeyAuthCode].GetString();
    if (!auth_code.empty()) {
      return true;
    }
  }
  return false;
}

bool GoogleSignIn::SecretJsonPopulated(rapidjson::Document& secret_doc) {
  auto obj = secret_doc.GetObject();
  if (obj.HasMember(kKeyInstalled)) {
    auto installed_obj = obj[kKeyInstalled].GetObject();
    bool client_id = installed_obj.HasMember(kKeyClientId) &&
                     installed_obj[kKeyClientId].IsString();
    bool project_id = installed_obj.HasMember(kKeyProjectId) &&
                      installed_obj[kKeyProjectId].IsString();
    bool auth_uri = installed_obj.HasMember(kKeyAuthUri) &&
                    installed_obj[kKeyAuthUri].IsString();
    bool token_uri = installed_obj.HasMember(kKeyTokenUri) &&
                     installed_obj[kKeyTokenUri].IsString();
    bool auth_provider_x509_cert_url =
        installed_obj.HasMember(kKeyAuthProviderX509CertUrl) &&
        installed_obj[kKeyAuthProviderX509CertUrl].IsString();
    bool client_secret = installed_obj.HasMember(kKeyClientSecret) &&
                         installed_obj[kKeyClientSecret].IsString();
    bool redirect_uris = installed_obj.HasMember(kKeyRedirectUris) &&
                         installed_obj[kKeyRedirectUris].IsArray();

    if (client_id && project_id && auth_uri && token_uri &&
        auth_provider_x509_cert_url && client_secret && redirect_uris) {
      return true;
    }
  }
  return false;
}

bool GoogleSignIn::CredentialsJsonPopulated(
    rapidjson::Document& credentials_doc) {
  auto obj = credentials_doc.GetObject();
  if (obj.HasMember(kKeyAccessToken) && obj[kKeyAccessToken].IsString() &&
      obj.HasMember(kKeyIdToken) && obj[kKeyIdToken].IsString() &&
      obj.HasMember(kKeyScope) && obj[kKeyScope].IsString() &&
      obj.HasMember(kKeyTokenType) && obj[kKeyTokenType].IsString() &&
      obj.HasMember(kKeyExpiresAt) && obj[kKeyExpiresAt].IsNumber() &&
      obj.HasMember(kKeyRefreshToken) && obj[kKeyRefreshToken].IsString() &&
      obj.HasMember(kKeyAuthCode) && obj[kKeyAuthCode].IsString()) {
    return true;
  }
  return false;
}

void GoogleSignIn::Init(std::vector<std::string>& requestedScopes,
                        std::string hostedDomain,
                        std::string signInOption,
                        std::string clientId,
                        std::string serverClientId,
                        bool forceCodeForRefreshToken) {
  SPDLOG_DEBUG("\trequestedScopes");
  for (auto& scope : requestedScopes) {
    SPDLOG_DEBUG("\t\t{}", scope);
  }
  SPDLOG_DEBUG("\thostedDomain: [{}]", hostedDomain);
  SPDLOG_DEBUG("\tsignInOption: [{}]", signInOption);
  SPDLOG_DEBUG("\tclientId: [{}]", clientId);
  SPDLOG_DEBUG("\tserverClientId: [{}]", serverClientId);
  SPDLOG_DEBUG("\tforceCodeForRefreshToken: {}", forceCodeForRefreshToken);

  auto secret_doc = GetClientSecret();
  if (!SecretJsonPopulated(secret_doc)) {
    spdlog::error(
        "Confirm client_secret JSON file has been downloaded from the Google "
        "cloud console");
  }
  auto credentials_doc = GetClientCredentials();
  if (!CredentialsJsonPopulated(credentials_doc)) {
    if (AuthCodeValuePresent(credentials_doc)) {
      credentials_doc = SwapAuthCodeForToken(secret_doc, credentials_doc);
    } else {
      CreateDefaultClientCredentialFile();
      secret_doc = GetClientSecret();
      std::string auth_uri = GetAuthUrl(secret_doc, requestedScopes);
      spdlog::error(
          "\tUpdate auth_code key in GOOGLE_API_OAUTH2_CLIENT_CREDENTIALS "
          "file with response from:\n\t{}",
          auth_uri);
    }
  } else {
    credentials_doc = RefreshToken(secret_doc, credentials_doc);
  }
}

std::unique_ptr<std::vector<uint8_t>> GoogleSignIn::GetUserData(
    const flutter::StandardMethodCodec& codec) {
  std::unique_ptr<std::vector<uint8_t>> result;

  auto credentials_doc = GetClientCredentials();
  if (CredentialsJsonPopulated(credentials_doc)) {
    auto o = credentials_doc.GetObject();
    std::string id_token = o[kKeyIdToken].GetString();
    std::string auth_code = o[kKeyAuthCode].GetString();
    std::string access_token = o[kKeyAccessToken].GetString();
    std::string token_type = o[kKeyTokenType].GetString();

    std::string auth_header =
        "Authorization: " + token_type + " " + access_token;
    std::vector<std::string> headers{"Content-Type: application/json",
                                     std::move(auth_header)};
    std::vector<std::pair<std::string, std::string>> url_form{};
    CurlClient client;
    std::string url = kPeopleUrl;
    client.Init(url, headers, url_form);
    auto response = client.RetrieveContent();

    if (client.GetCode() != CURLE_OK) {
      spdlog::error("[google_sign_in] curl failure {} - {}",
                    static_cast<int>(client.GetCode()), response);
      result = codec.EncodeErrorEnvelope("http_client_failure", "");
      return std::move(result);
    }

    rapidjson::Document doc;
    doc.Parse(response.c_str());
    if (doc.GetParseError() != rapidjson::kParseErrorNone) {
      spdlog::error("[google_sign_in] curl response parse failure: {} - {}",
                    static_cast<int>(doc.GetParseError()), response);
      result = codec.EncodeErrorEnvelope("parse_error", "");
      return std::move(result);
    }

    auto resp = doc.GetObject();

    // check for error
    if (resp.HasMember("error") && resp["error"].IsObject()) {
      auto obj = resp["error"].GetObject();
      if (obj.HasMember("code") && obj["code"].IsNumber() &&
          obj.HasMember("message") && obj["message"].IsString() &&
          obj.HasMember("status") && obj["status"].IsString()) {
        int code = obj["code"].GetInt();
        std::string message = obj["message"].GetString();
        std::string status = obj["status"].GetString();
        spdlog::error("[google_sign_in] [{}] {}: {}", code, status, message);
        result = codec.EncodeErrorEnvelope(status.c_str(), message.c_str());
        return std::move(result);
      }
    }

    std::string id;
    if (resp.HasMember(kKeyResourceName) && resp[kKeyResourceName].IsString()) {
      id = resp[kKeyResourceName].GetString();
    }

    std::string email;
    if (resp.HasMember(kKeyEmailAddresses) &&
        resp[kKeyEmailAddresses].IsArray()) {
      auto arr = resp[kKeyEmailAddresses].GetArray();
      auto index0 = arr[0].GetObject();
      if (index0.HasMember(kKeyMetadata) && index0[kKeyMetadata].IsObject() &&
          index0.HasMember(kKeyValue) && index0[kKeyValue].IsString()) {
        auto meta_obj = index0[kKeyMetadata].GetObject();
        if (meta_obj.HasMember(kKeySourcePrimary) &&
            meta_obj[kKeySourcePrimary].IsBool() &&
            meta_obj[kKeySourcePrimary].GetBool()) {
          email = index0[kKeyValue].GetString();
        }
      }
    }

    std::string display_name;
    if (resp.HasMember(kKeyNames) && resp[kKeyNames].IsArray()) {
      auto arr = resp[kKeyNames].GetArray();
      auto index0 = arr[0].GetObject();
      if (index0.HasMember(kKeyMetadata) && index0[kKeyMetadata].IsObject() &&
          index0.HasMember(kKeyDisplayName) &&
          index0[kKeyDisplayName].IsString()) {
        auto meta_obj = index0[kKeyMetadata].GetObject();
        if (meta_obj.HasMember(kKeySourcePrimary) &&
            meta_obj[kKeySourcePrimary].IsBool() &&
            meta_obj[kKeySourcePrimary].GetBool()) {
          display_name = index0[kKeyDisplayName].GetString();
        }
      }
    }

    std::string photo_url;
    if (resp.HasMember(kKeyPhotos) && resp[kKeyPhotos].IsArray()) {
      auto arr = resp[kKeyPhotos].GetArray();
      auto index0 = arr[0].GetObject();
      if (index0.HasMember(kKeyMetadata) && index0[kKeyMetadata].IsObject() &&
          index0.HasMember(kKeyUrl) && index0[kKeyUrl].IsString()) {
        auto meta_obj = index0[kKeyMetadata].GetObject();
        if (meta_obj.HasMember(kKeyPrimary) && meta_obj[kKeyPrimary].IsBool() &&
            meta_obj[kKeyPrimary].GetBool()) {
          photo_url = index0[kKeyUrl].GetString();
        }
      }
    }
    SPDLOG_DEBUG("id: {}", id);
    SPDLOG_DEBUG("email: {}", email);
    SPDLOG_DEBUG("display_name: {}", display_name);
    SPDLOG_DEBUG("photo_url: {}", photo_url);

    auto res = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kMethodResponseKeyId),
         flutter::EncodableValue(id)},
        {flutter::EncodableValue(kMethodResponseKeyEmail),
         flutter::EncodableValue(email)},
        {flutter::EncodableValue(kKeyDisplayName),
         flutter::EncodableValue(display_name)},
        {flutter::EncodableValue(kMethodResponseKeyPhotoUrl),
         flutter::EncodableValue(photo_url)},
        {flutter::EncodableValue(kMethodResponseKeyServerAuthCode),
         flutter::EncodableValue(auth_code.c_str())},
        {flutter::EncodableValue(kMethodResponseKeyIdToken),
         flutter::EncodableValue(id_token.c_str())},
    });
    result = codec.EncodeSuccessEnvelope(&res);
  } else {
    result = codec.EncodeErrorEnvelope("authentication_failure", "");
  }
  return std::move(result);
}

std::unique_ptr<std::vector<uint8_t>> GoogleSignIn::GetTokens(
    std::string /* email */,
    bool /* shouldRecoverAuth */,
    const flutter::StandardMethodCodec& codec) {
  std::unique_ptr<std::vector<uint8_t>> result;

  auto credentials_doc = GetClientCredentials();
  if (CredentialsJsonPopulated(credentials_doc)) {
    auto o = credentials_doc.GetObject();
    std::string access_token = o[kKeyAccessToken].GetString();
    std::string id_token = o[kKeyIdToken].GetString();
    std::string auth_code = o[kKeyAuthCode].GetString();

    auto res = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kMethodResponseKeyServerAuthCode),
         flutter::EncodableValue(auth_code.c_str())},
        {flutter::EncodableValue(kMethodResponseKeyIdToken),
         flutter::EncodableValue(id_token.c_str())},
        {flutter::EncodableValue(kMethodResponseKeyAccessToken),
         flutter::EncodableValue(access_token.c_str())}});
    result = codec.EncodeSuccessEnvelope(&res);
  } else {
    result = codec.EncodeErrorEnvelope("authentication_failure", "");
  }
  return std::move(result);
}

void GoogleSignIn::OnPlatformMessage(const FlutterPlatformMessage* message,
                                     void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kMethodInit) {
    SPDLOG_DEBUG("[google_sign_in] <init>");

    std::string signInOption;
    std::vector<std::string> requestedScopes;
    std::string hostedDomain;
    std::string clientId;
    std::string serverClientId;
    bool forceCodeForRefreshToken{};

    if (obj->arguments()->IsNull()) {
      result = codec.EncodeErrorEnvelope("invalid_arguments", "");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }

    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kMethodArgSignInOption && !it.second.IsNull()) {
        signInOption.assign(std::get<std::string>(it.second));
      } else if (key == kMethodArgScopes && !it.second.IsNull()) {
        auto requestedScopes_ =
            std::get<std::vector<flutter::EncodableValue>>(it.second);
        for (auto& scope : requestedScopes_) {
          if (!scope.IsNull()) {
            auto val = std::get<std::string>(scope);
            requestedScopes.push_back(std::move(val));
          }
        }
      } else if (key == kMethodArgHostedDomain && !it.second.IsNull()) {
        hostedDomain.assign(std::get<std::string>(it.second));
      } else if (key == kMethodArgClientId && !it.second.IsNull()) {
        clientId.assign(std::get<std::string>(it.second));
      } else if (key == kMethodArgServerClientId && !it.second.IsNull()) {
        serverClientId.assign(std::get<std::string>(it.second));
      } else if (key == kMethodArgForceCodeForRefreshToken &&
                 !it.second.IsNull()) {
        forceCodeForRefreshToken = std::get<bool>(it.second);
      }
    }

    Init(requestedScopes, hostedDomain, signInOption, clientId, serverClientId,
         forceCodeForRefreshToken);
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kMethodSignIn) {
    SPDLOG_DEBUG("[google_sign_in] <signIn>");
    result = GetUserData(codec);
  } else if (method == kMethodSignInSilently) {
    SPDLOG_DEBUG("[google_sign_in] <signInSilently>");
    result = GetUserData(codec);
  } else if (method == kMethodGetTokens) {
    SPDLOG_DEBUG("[google_sign_in] <getTokens>");
    if (obj->arguments()->IsNull()) {
      result = codec.EncodeErrorEnvelope("invalid_arguments", "");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }
    std::string email;
    bool shouldRecoverAuth{};
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kMethodResponseKeyEmail && !it.second.IsNull()) {
        email.assign(std::get<std::string>(it.second));
      } else if (key == kMethodArgShouldRecoverAuth && !it.second.IsNull()) {
        shouldRecoverAuth = std::get<bool>(it.second);
      }
    }
    SPDLOG_DEBUG("\temail: [{}]", email);
    SPDLOG_DEBUG("\tshouldRecoverAuth: {}", shouldRecoverAuth);
    result = GetTokens(std::move(email), shouldRecoverAuth, codec);
  } else if (method == kMethodSignOut) {
    SPDLOG_DEBUG("[google_sign_in] <signOut>");
    result = GetUserData(codec);
  } else if (method == kMethodDisconnect) {
    SPDLOG_DEBUG("[google_sign_in] <disconnect>");
    result = GetUserData(codec);
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
