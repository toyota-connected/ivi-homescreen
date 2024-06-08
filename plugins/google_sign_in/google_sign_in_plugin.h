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

#ifndef FLUTTER_PLUGIN_GOOGLE_SIGN_IN_PLUGIN_H_
#define FLUTTER_PLUGIN_GOOGLE_SIGN_IN_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.h"

namespace google_sign_in_plugin {

class GoogleSignInPlugin final : public flutter::Plugin,
                                 public GoogleSignInApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  GoogleSignInPlugin() = default;

  ~GoogleSignInPlugin() override = default;

  /**
   * @brief Function to return client secret object
   * @return rapidjson::Document
   * @retval Returns document of client secret file, empty object if error
   * @relation
   * google_sign_in
   */
  static rapidjson::Document GetClientSecret();

  /**
   * @brief Function to update client credential file
   * @return rapidjson::Document
   * @retval Returns document of client credentials file, empty object if error
   * @relation
   * google_sign_in
   */
  static rapidjson::Document GetClientCredentials();

  /**
   * @brief Function to update client credential file
   * @param client_credential_doc document of credential object
   * @return bool
   * @retval Returns true if file has been updated, false otherwise
   * @relation
   * google_sign_in
   */
  static bool UpdateClientCredentialFile(
      const rapidjson::Document& client_credential_doc);

  /**
   * @brief Function to swap authorization code for OAuth2 token
   * @param client_secret_doc document of secret object
   * @param client_credential_doc document of credential object
   * @return rapidjson::Document
   * @retval Returns empty object if failed, or populated object if refreshed
   * @relation
   * google_sign_in
   */
  static rapidjson::Document SwapAuthCodeForToken(
      rapidjson::Document& client_secret_doc,
      rapidjson::Document& client_credential_doc);

  /**
   * @brief Function to refresh the OAuth2 token
   * @param client_secret_doc document of secret object
   * @param client_credential_doc document of credential object
   * @return rapidjson::Document
   * @retval Returns credential object. empty object if failed
   * @relation
   * google_sign_in
   */
  static rapidjson::Document RefreshToken(
      rapidjson::Document& client_secret_doc,
      rapidjson::Document& client_credential_doc);

  /**
   * @brief Function to create the default credential file
   * @return bool
   * @retval Returns true if file has been created, otherwise false
   * @relation
   * google_sign_in
   */
  static bool CreateDefaultClientCredentialFile();

  /**
   * @brief Function to construct Authorization URL
   * @param[in] secret_doc rapidjson document pointer to credentials
   * @param[in] scopes vector of strings containing desired scopes
   * @return std::string
   * @relation
   * google_sign_in
   */
  static std::string GetAuthUrl(rapidjson::Document& secret_doc,
                                const std::vector<std::string>& scopes);

  /**
   * @brief Function to determine if auth_code key is present in document
   * @param[in] credentials_doc rapidjson document pointer to credentials
   * @return bool
   * @relation
   * google_sign_in
   */
  static bool AuthCodeValuePresent(rapidjson::Document& credentials_doc);

  /**
   * @brief Function to validate secret object
   * @param secret_doc document of secret object
   * @return bool
   * @retval Returns true if secret object is valid, false if not
   * @relation
   * google_sign_in
   */
  static bool SecretJsonPopulated(rapidjson::Document& secret_doc);

  /**
   * @brief Function to validate credentials object
   * @param credentials_doc document of credentials object
   * @return bool
   * @retval Returns true if secret object is valid, false if not
   * @relation
   * google_sign_in
   */
  static bool CredentialsJsonPopulated(rapidjson::Document& credentials_doc);

  /**
   * @brief GetTokens
   * @param requestedScopes vector of strings - requested scopes to authorize
   * with
   * @param hostedDomain hosted domain
   * @param signInOption sign in option
   * @param clientId client id
   * @param serverClientId server client id
   * @param forceCodeForRefreshToken boot
   * @return void
   * @relation
   * google_sign_in
   */
  void Init(const std::vector<std::string>& requestedScopes,
            std::string hostedDomain,
            std::string signInOption,
            std::string clientId,
            std::string serverClientId,
            bool forceCodeForRefreshToken) override;

  /**
   * @brief GetUserData
   * @param codec standard method code pointer
   * @return std::unique_ptr<std::vector<uint8_t>>
   * @retval Returns value suitable to send to engine
   * @relation
   * google_sign_in
   */
  flutter::EncodableValue GetUserData() override;

  /**
   * @brief GetTokens
   * @param email
   * @param shouldRecoverAuth
   * @param codec standard method code pointer
   * @return std::unique_ptr<std::vector<uint8_t>>
   * @retval Returns value suitable to send to engine
   * @relation
   * google_sign_in
   */
  flutter::EncodableValue GetTokens(const std::string& email,
                                    bool shouldRecoverAuth) override;

  // Disallow copy and assign.
  GoogleSignInPlugin(const GoogleSignInPlugin&) = delete;
  GoogleSignInPlugin& operator=(const GoogleSignInPlugin&) = delete;

 private:
  // Method Response Constants
  static constexpr const char* kMethodResponseKeyAccessToken = "accessToken";
  static constexpr const char* kMethodResponseKeyEmail = "email";
  static constexpr const char* kMethodResponseKeyId = "id";
  static constexpr const char* kMethodResponseKeyIdToken = "idToken";
  static constexpr const char* kMethodResponseKeyPhotoUrl = "photoUrl";
  static constexpr const char* kMethodResponseKeyServerAuthCode =
      "serverAuthCode";
};
}  // namespace google_sign_in_plugin

#endif  // FLUTTER_PLUGIN_GOOGLE_SIGN_IN_PLUGIN_H_