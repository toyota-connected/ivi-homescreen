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

#include "audio_players.h"

#include <memory>
#include <vector>

#include <flutter/standard_method_codec.h>

#include "audio_player.h"
#include "audio_players_registry.h"
#include "engine.h"
#include "utils.h"

namespace Plugins {

static void OnError(Engine const* engine,
                    const FlutterPlatformMessageResponseHandle* handle,
                    const std::string& error_code,
                    const std::string& error_message = "") {
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto encoded = codec.EncodeErrorEnvelope(error_code, error_message);
  engine->SendPlatformMessageResponse(handle, encoded->data(), encoded->size());
}

static std::string GetStringValue(
    const char* key,
    const std::map<flutter::EncodableValue, flutter::EncodableValue>* args) {
  std::string result;
  for (auto& it : *args) {
    if (key == std::get<std::string>(it.first) &&
        std::holds_alternative<std::string>(it.second)) {
      result = std::get<std::string>(it.second);
      break;
    }
  }
  return std::move(result);
}

void AudioPlayers::OnPlatformMessage(const FlutterPlatformMessage* message,
                                     void* userdata) {
  flutter::EncodableValue result;
  auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  if (obj->method_name().empty() ||
      !std::holds_alternative<flutter::EncodableMap>(*obj->arguments())) {
    OnError(engine, message->response_handle, "invalid_arguments");
    return;
  }
  auto method = obj->method_name();
  SPDLOG_TRACE("[audio_players] {} on {}", method, message->channel);

  auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

  try {
    if (method == kMethodCreate) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: {}", playerId);
      AudioPlayersRegistry::GetInstance().AddAudioPlayer(
          playerId, std::make_unique<AudioPlayer>(playerId, engine));

    } else if (method == kMethodDispose) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId)->Dispose();
      AudioPlayersRegistry::GetInstance().RemoveAudioPlayer(playerId);

    } else if (method == kMethodSetReleaseMode) {
      std::string playerId;
      std::string releaseMode;
      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyReleaseMode == std::get<std::string>(it.first) &&
                   std::holds_alternative<std::string>(it.second)) {
          releaseMode = std::get<std::string>(it.second);
        }
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\treleaseMode: [{}]", releaseMode);

      auto looping = releaseMode.find("loop") != std::string::npos;
      auto player =
          AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId);
      player->SetLooping(looping);

    } else if (method == kMethodSeek) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      int32_t position = 0;
      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyPosition == std::get<std::string>(it.first) &&
                   std::holds_alternative<int32_t>(it.second)) {
          position = std::get<int32_t>(it.second);
        }
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\tposition: [{}]", position);

      auto player =
          AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId);
      player->SetPosition(position);

    } else if (method == kMethodSetSourceUrl) {
      std::string playerId;
      std::string url;
      bool isLocal{};

      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyIsLocal == std::get<std::string>(it.first) &&
                   std::holds_alternative<bool>(it.second)) {
          isLocal = std::get<bool>(it.second);
        } else if (kKeyUrl == std::get<std::string>(it.first) &&
                   std::holds_alternative<std::string>(it.second)) {
          url = std::get<std::string>(it.second);
        }
      }

      if (isLocal) {
        url = std::string("file://") + url;
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\tisLocal: [{}]", isLocal);
      SPDLOG_TRACE("\turl: [{}]", url);

      AudioPlayersRegistry::GetInstance()
          .GetAudioPlayer(playerId)
          ->SetSourceUrl(url, isLocal);

    } else if (method == kMethodSetVolume) {
      std::string playerId;
      double volume = 1.0;
      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyVolume == std::get<std::string>(it.first) &&
                   std::holds_alternative<double>(it.second)) {
          volume = std::get<double>(it.second);
        }
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\tvolume: [{}]", volume);

      auto player =
          AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId);
      player->SetVolume(volume);

    } else if (method == kMethodSetBalance) {
      std::string playerId;
      double balance = 0.0;
      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyBalance == std::get<std::string>(it.first) &&
                   std::holds_alternative<double>(it.second)) {
          balance = std::get<double>(it.second);
        }
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\tbalance: [{}]", balance);

      auto player =
          AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId);
      player->SetBalance(static_cast<float>(balance));

    } else if (method == kMethodSetPlaybackRate) {
      std::string playerId;
      double playback_rate = 1.0;
      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyPlaybackRate == std::get<std::string>(it.first) &&
                   std::holds_alternative<double>(it.second)) {
          playback_rate = std::get<double>(it.second);
        }
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\tplayback_rate: [{}]", playback_rate);

      AudioPlayersRegistry::GetInstance()
          .GetAudioPlayer(playerId)
          ->SetPlaybackRate(playback_rate);

    } else if (method == kMethodSetPlayerMode) {
      std::string playerId;
      std::string playerMode;
      for (auto& it : *args) {
        if (kKeyPlayerId == std::get<std::string>(it.first) &&
            std::holds_alternative<std::string>(it.second)) {
          playerId = std::get<std::string>(it.second);
        } else if (kKeyPlayerMode == std::get<std::string>(it.first) &&
                   std::holds_alternative<std::string>(it.second)) {
          playerMode = std::get<std::string>(it.second);
        }
      }

      SPDLOG_TRACE("\tplayerId: [{}]", playerId);
      SPDLOG_TRACE("\tplayerMode: [{}]", playerMode);

      AudioPlayersRegistry::GetInstance()
          .GetAudioPlayer(playerId)
          ->SetPlayerMode(std::move(playerMode));

    } else if (method == kMethodPause) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

      AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId)->Pause();

    } else if (method == kMethodResume) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

      AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId)->Resume();

    } else if (method == kMethodStop) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

      auto player =
          AudioPlayersRegistry::GetInstance().GetAudioPlayer(playerId);
      player->Pause();
      player->SetPosition(0);

    } else if (method == kMethodRelease) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

      AudioPlayersRegistry::GetInstance()
          .GetAudioPlayer(playerId)
          ->ReleaseMediaSource();

    } else if (method == kMethodGetCurrentPosition) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

      auto position = AudioPlayersRegistry::GetInstance()
                          .GetAudioPlayer(playerId)
                          ->GetPosition();
      if (position.has_value()) {
        result = position.value();
        SPDLOG_TRACE("position: {}", position.value());
      }
    } else if (method == kMethodGetDuration) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

      auto duration = AudioPlayersRegistry::GetInstance()
                          .GetAudioPlayer(playerId)
                          ->GetDuration();
      if (duration.has_value()) {
        result = duration.value();
        SPDLOG_TRACE("\tduration: [{}]", duration.value());
      }

    } else if (method == kMethodSetAudioContext) {
      std::string playerId = GetStringValue(kKeyPlayerId, args);
      SPDLOG_TRACE("\tplayerId: [{}]", playerId);

    } else {
      Utils::PrintFlutterEncodableValue(method.c_str(), *args);
      OnError(engine, message->response_handle, "unimplemented",
              "method not implemented");
      return;
    }

    std::unique_ptr<std::vector<uint8_t>> encoded;

    if (!result.IsNull()) {
      encoded = codec.EncodeSuccessEnvelope(&result);
    } else {
      encoded = codec.EncodeSuccessEnvelope();
    }
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());

  } catch (const gchar* error) {
    OnError(engine, message->response_handle, "LinuxAudioError", error);
  } catch (const std::runtime_error& e) {
    OnError(engine, message->response_handle, "LinuxAudioError", e.what());
  } catch (...) {
    std::exception_ptr p = std::current_exception();
    OnError(engine, message->response_handle, "LinuxAudioError",
            p ? p.__cxa_exception_type()->name()
              : "Unknown AudioPlayersLinux error");
  }
}
}
