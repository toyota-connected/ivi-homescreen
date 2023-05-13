/*
 * Copyright 2020 Toyota Connected North America
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

#include "secure_storage.h"

#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "logging.h"

static SecureStorage* pInstance_;
static std::mutex mutex_;

void SecureStorage::deleteIt(const char* key) {
  GetInstance("keyring")->keyring_.deleteItem(key);
}

void SecureStorage::deleteAll() {
  GetInstance("keyring")->keyring_.deleteKeyring();
}

void SecureStorage::write(const char* key, const char* value) {
  GetInstance("keyring")->keyring_.addItem(key, value);
}

flutter::EncodableValue SecureStorage::read(const char* key) {
  auto str = GetInstance("keyring")->keyring_.getItem(key);
  if (str.empty()) {
    return flutter::EncodableValue();
  }
  return flutter::EncodableValue(str);
}

flutter::EncodableValue SecureStorage::readAll() {
  auto result = flutter::EncodableMap{};
  auto document = GetInstance("keyring")->keyring_.readFromKeyring();
  if (document.IsObject()) {
    for (rapidjson::Value::ConstMemberIterator itr = document.MemberBegin();
         itr != document.MemberEnd(); ++itr) {
      result.emplace(flutter::EncodableValue(itr->name.GetString()),
                     flutter::EncodableValue(itr->value.GetString()));
    }
  }
  return flutter::EncodableValue(result);
}

flutter::EncodableValue SecureStorage::containsKey(const char* key) {
  auto document = GetInstance("keyring")->keyring_.readFromKeyring();
  return flutter::EncodableValue(document.HasMember(key));
}

void SecureStorage::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (obj->arguments()->IsNull()) {
    result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    engine->SendPlatformMessageResponse(message->response_handle,
                                        result->data(), result->size());
    return;
  }

  auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

  std::string keyString;
  auto it = args->find(flutter::EncodableValue(kKey));
  if (it != args->end() && !it->second.IsNull()) {
    keyString = std::get<std::string>(it->second);
  }

  std::string valueString;
  it = args->find(flutter::EncodableValue(kValue));
  if (it != args->end() && !it->second.IsNull()) {
    valueString = std::get<std::string>(it->second);
  }

  if (method == kWrite) {
    DLOG(INFO) << "secure_storage: [Write] key: " << keyString
               << ", value: " << valueString;
    write(keyString.c_str(), valueString.c_str());
    auto val = flutter::EncodableValue(true);
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == kRead) {
    DLOG(INFO) << "secure_storage: [Read] key: " << keyString;
    auto val = read(keyString.c_str());
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == kReadAll) {
    DLOG(INFO) << "secure_storage: [ReadAll]";
    auto val = readAll();
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == kDelete) {
    DLOG(INFO) << "secure_storage: [Delete]";
    deleteIt(keyString.c_str());
    auto val = flutter::EncodableValue(true);
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == kDeleteAll) {
    DLOG(INFO) << "secure_storage: [DeleteAll]";
    deleteAll();
    auto val = flutter::EncodableValue(true);
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == kContainsKey) {
    DLOG(INFO) << "secure_storage: [ContainsKey]";
    auto val = containsKey(keyString.c_str());
    result = codec.EncodeSuccessEnvelope(&val);
  } else {
    DLOG(ERROR) << "secure_storage: " << method << " is unhandled";
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

SecureStorage* SecureStorage::GetInstance(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pInstance_ == nullptr) {
    pInstance_ = new SecureStorage(value);
  }
  return pInstance_;
}
