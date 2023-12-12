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

#pragma once

#include <string>

#include <flutter/standard_method_codec.h>
#include <rapidjson/document.h>
#include <shell/platform/embedder/embedder.h>

#include "curl_client/curl_client.h"

class GoogleSignIn {
 public:
  static constexpr char kChannelName[] = "plugins.flutter.io/google_sign_in";

  /**
   * @brief Callback function for platform messages about google_sign_in
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

 private:
  static constexpr const char* kPeopleUrl =
      "https://people.googleapis.com/v1/people/"
      "me?personFields=photos,names,emailAddresses";

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
  static constexpr const char* kMethodArgShouldRecoverAuth =
      "shouldRecoverAuth";

  static constexpr const char* kClientCredentialsPathEnvironmentVariable =
      "GOOGLE_API_OAUTH2_CLIENT_CREDENTIALS";
  static constexpr const char* kClientSecretPathEnvironmentVariable =
      "GOOGLE_API_OAUTH2_CLIENT_SECRET_JSON";

  // Method Response Constants
  static constexpr const char* kMethodResponseKeyAccessToken = "accessToken";
  static constexpr const char* kMethodResponseKeyEmail = "email";
  static constexpr const char* kMethodResponseKeyId = "id";
  static constexpr const char* kMethodResponseKeyIdToken = "idToken";
  static constexpr const char* kMethodResponseKeyPhotoUrl = "photoUrl";
  static constexpr const char* kMethodResponseKeyServerAuthCode =
      "serverAuthCode";

  // Key Constants
  static constexpr const char* kKeyAccessToken = "access_token";
  static constexpr const char* kKeyAuthCode = "auth_code";
  static constexpr const char* kKeyAuthProviderX509CertUrl =
      "auth_provider_x509_cert_url";
  static constexpr const char* kKeyAuthUri = "auth_uri";
  static constexpr const char* kKeyClientId = "client_id";
  static constexpr const char* kKeyClientSecret = "client_secret";
  static constexpr const char* kKeyCode = "code";
  static constexpr const char* kKeyExpiresAt = "expires_at";
  static constexpr const char* kKeyExpiresIn = "expires_in";
  static constexpr const char* kKeyGrantType = "grant_type";
  static constexpr const char* kKeyIdToken = "id_token";
  static constexpr const char* kKeyInstalled = "installed";
  static constexpr const char* kKeyProjectId = "project_id";
  static constexpr const char* kKeyRefreshToken = "refresh_token";
  static constexpr const char* kKeyRedirectUri = "redirect_uri";
  static constexpr const char* kKeyRedirectUris = "redirect_uris";
  static constexpr const char* kKeyScope = "scope";
  static constexpr const char* kKeyTokenType = "token_type";
  static constexpr const char* kKeyTokenUri = "token_uri";

  /// Value Constants
  static constexpr const char* kValueAuthorizationCode = "authorization_code";
  static constexpr const char* kValueRedirectUri = "urn:ietf:wg:oauth:2.0:oob";
  static constexpr const char* kValueRefreshToken = "refresh_token";

  /// People Response Constants
  static constexpr const char* kKeyDisplayName = "displayName";
  static constexpr const char* kKeyEmailAddresses = "emailAddresses";
  static constexpr const char* kKeyMetadata = "metadata";
  static constexpr const char* kKeyNames = "names";
  static constexpr const char* kKeyPhotos = "photos";
  static constexpr const char* kKeyPrimary = "primary";
  static constexpr const char* kKeyResourceName = "resourceName";
  static constexpr const char* kKeySourcePrimary = "sourcePrimary";
  static constexpr const char* kKeyUrl = "url";
  static constexpr const char* kKeyValue = "value";

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
  static void Init(const std::vector<std::string>& requestedScopes,
                   std::string hostedDomain,
                   std::string signInOption,
                   std::string clientId,
                   std::string serverClientId,
                   bool forceCodeForRefreshToken);

  /**
   * @brief GetUserData
   * @param codec standard method code pointer
   * @return std::unique_ptr<std::vector<uint8_t>>
   * @retval Returns value suitable to send to engine
   * @relation
   * google_sign_in
   */
  static std::unique_ptr<std::vector<uint8_t>> GetUserData(
      const flutter::StandardMethodCodec& codec);

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
  static std::unique_ptr<std::vector<uint8_t>> GetTokens(
      const std::string& email,
      bool shouldRecoverAuth,
      const flutter::StandardMethodCodec& codec);
};
