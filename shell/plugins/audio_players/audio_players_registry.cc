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

#include "audio_players_registry.h"

#include <memory>
#include <mutex>

#include <flutter/standard_method_codec.h>
#include <flutter/standard_message_codec.h>

#include "audio_player.h"
#include "platform_channel.h"
#include "logging/logging.h"

namespace Plugins {

std::shared_ptr<AudioPlayersRegistry> AudioPlayersRegistry::sInstance = nullptr;

std::mutex gAudioPlayersRegistryMutex;

AudioPlayersRegistry::AudioPlayersRegistry() {
  gst_init(nullptr, nullptr);

  const auto platform_channel = PlatformChannel::GetInstance();
  platform_channel->RegisterCallback(kGlobalChannel, OnPlatformMessageGlobal);
  platform_channel->RegisterCallback(kEventChannel, OnPlatformMessageEvents);
}

AudioPlayersRegistry::~AudioPlayersRegistry() = default;

void AudioPlayersRegistry::AddAudioPlayer(
    const std::string& id,
    std::unique_ptr<AudioPlayer> audio_player) {
  const std::scoped_lock lock(gAudioPlayersRegistryMutex);
  registry_[id] = std::move(audio_player);
}

AudioPlayer* AudioPlayersRegistry::GetAudioPlayer(const std::string& id) {
  const std::scoped_lock lock(gAudioPlayersRegistryMutex);
  if (registry_.find(id) == registry_.end()) {
    return nullptr;
  }
  return registry_[id].get();
}

void AudioPlayersRegistry::RemoveAudioPlayer(const std::string& id) {
  const std::scoped_lock lock(gAudioPlayersRegistryMutex);
  const auto it = registry_.find(id);
  if (it != registry_.end()) {
    registry_[id].reset();
    registry_.erase(it);
  }
}

void AudioPlayersRegistry::on_global_log(const Engine* engine,
                                         const gchar* message) {
  const flutter::EncodableValue map(flutter::EncodableMap(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onLog")},
       {flutter::EncodableValue("value"), flutter::EncodableValue(message)}}));
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto encoded = codec.EncodeMessage(map);
  spdlog::debug("[on_global_log] audio.onLog: {}", message);
  engine->SendPlatformMessage(kEventChannel, std::move(encoded));
}

void AudioPlayersRegistry::on_global_error(const Engine* engine,
                                           const std::string& code,
                                           const std::string& message) {
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto encoded = codec.EncodeErrorEnvelope(code, message);
  spdlog::debug("[on_global_error] ({}) {}", code, message);
  engine->SendPlatformMessage(kGlobalChannel, std::move(encoded));
}

void AudioPlayersRegistry::OnPlatformMessageGlobal(
    const FlutterPlatformMessage* message,
    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto method =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method_name = method->method_name();
  const auto args = std::get_if<flutter::EncodableMap>(method->arguments());

  SPDLOG_DEBUG("[audioplayers.global] {}", method_name);

  on_global_log(engine, "Setting AudioContext is not supported on Linux");
  if (kMethodSetAudioContext == method_name) {
    SPDLOG_DEBUG("\tsetAudioContext");
    const auto res = flutter::EncodableValue(1);
    result = codec.EncodeSuccessEnvelope(&res);
  } else if (kMethodEmitLog == method_name) {
    SPDLOG_DEBUG("\temitLog");
    std::string msg;
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (kKeyMessage == std::get<std::string>(it.first) &&
          std::holds_alternative<std::string>(it.second)) {
        msg = std::get<std::string>(it.second);
      }
    }
    on_global_log(engine, msg.c_str());
    const auto res = flutter::EncodableValue(1);
    result = codec.EncodeSuccessEnvelope(&res);
  } else if (kMethodEmitError == method_name) {
    SPDLOG_DEBUG("\temitError");
    std::string msg;
    std::string code;
    for (auto& it : *args) {
      if (kKeyMessage == std::get<std::string>(it.first) &&
          std::holds_alternative<std::string>(it.second)) {
        msg = std::get<std::string>(it.second);
      } else if (kKeyCode == std::get<std::string>(it.first) &&
                 std::holds_alternative<std::string>(it.second)) {
        code = std::get<std::string>(it.second);
      }
    }
    on_global_error(engine, code, msg);
    const auto res = flutter::EncodableValue(1);
    result = codec.EncodeSuccessEnvelope(&res);
  } else {
    Utils::PrintFlutterEncodableValue(method_name.c_str(), *args);
    result =
        codec.EncodeErrorEnvelope("unimplemented", "method not implemented");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void AudioPlayersRegistry::OnPlatformMessageEvents(
    const FlutterPlatformMessage* message,
    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto method =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method_name = method->method_name();
  SPDLOG_DEBUG("[audioplayers.global/events] {}", method_name);

  if (kMethodListen == method_name) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    const auto args = std::get_if<flutter::EncodableMap>(method->arguments());
    Utils::PrintFlutterEncodableValue(method_name.c_str(), *args);
    result =
        codec.EncodeErrorEnvelope("unimplemented", "method not implemented");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
}  // namespace Plugins
