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

#include <memory>

#include <flutter/standard_message_codec.h>

#include "audio_player.h"
#include "audio_players.h"
#include "audio_players_context.h"

AudioPlayersContext::AudioPlayersContext() {
  gst_init(nullptr, nullptr);

  auto platform_channel = PlatformChannel::GetInstance();
  platform_channel->RegisterCallback(kGlobalChannel, OnPlatformMessageGlobal);
  platform_channel->RegisterCallback(kEventChannel, OnPlatformMessageEvents);
}

AudioPlayersContext::~AudioPlayersContext() {}

void AudioPlayersContext::AddAudioPlayer(
    std::string id,
    std::unique_ptr<AudioPlayer> audio_player) {
  registry_[id] = std::move(audio_player);
}

AudioPlayer* AudioPlayersContext::GetAudioPlayer(std::string id) {
  const auto it = registry_.find(id);
  if (it != registry_.end()) {
    return registry_[id].get();
  } else {
    return nullptr;
  };
}

void AudioPlayersContext::RemoveAudioPlayer(std::string id) {
  const auto it = registry_.find(id);
  if (it != registry_.end())
    registry_.erase(it);
}

void AudioPlayersContext::on_global_log(Engine* engine, const gchar* message) {
  flutter::EncodableValue map(flutter::EncodableMap(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onLog")},
       {flutter::EncodableValue("value"), flutter::EncodableValue(message)}}));
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto encoded = codec.EncodeMessage(map);
  engine->GetPlatformRunner()->QueuePlatformMessage(kEventChannel, std::move(encoded));
}

void AudioPlayersContext::OnPlatformMessageGlobal(
    const FlutterPlatformMessage* message,
    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

  SPDLOG_DEBUG("[audioplayers.global] {}:", method);

  on_global_log(engine, "Setting AudioContext is not supported on Linux");
  if (method == AudioPlayers::kMethodSetAudioContext) {
    SPDLOG_DEBUG("\tsetAudioContext");
    auto res = flutter::EncodableValue(1);
    result = codec.EncodeSuccessEnvelope(&res);
  } else if (method == AudioPlayers::kMethodEmitLog) {
    SPDLOG_DEBUG("\temitLog");
    std::string message;
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == "message" && std::holds_alternative<std::string>(it.second)) {
        message = std::get<std::string>(it.second);
      }
    }
    on_global_log(engine, message.c_str());
    auto res = flutter::EncodableValue(1);
    result = codec.EncodeSuccessEnvelope(&res);
  } else if (method == AudioPlayers::kMethodEmitError) {
    SPDLOG_DEBUG("\temitError");
    std::string message;
    std::string code;
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == "message" && std::holds_alternative<std::string>(it.second)) {
        message = std::get<std::string>(it.second);
      } else if (key == "code" &&
                 std::holds_alternative<std::string>(it.second)) {
        code = std::get<std::string>(it.second);
      }
    }
    // fl_event_channel_send_error(globalEvents, code, message, nullptr,
    // nullptr, nullptr);
    auto res = flutter::EncodableValue(1);
    result = codec.EncodeSuccessEnvelope(&res);
  } else {
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    Utils::PrintFlutterEncodableValue(method.c_str(), *args);
    result =
        codec.EncodeErrorEnvelope("unimplemented", "method not implemented");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void AudioPlayersContext::OnPlatformMessageEvents(
    const FlutterPlatformMessage* message,
    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  SPDLOG_DEBUG("[audioplayers.global/events] {}:", method);

  if (method == AudioPlayers::kMethodListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    Utils::PrintFlutterEncodableValue(method.c_str(), *args);
    result =
        codec.EncodeErrorEnvelope("unimplemented", "method not implemented");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
