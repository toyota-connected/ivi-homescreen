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

#undef _HAS_EXCEPTIONS

#include "messages.h"

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <optional>
#include <string>

#include "audioplayers_linux_plugin.h"

#include "logging/logging.h"
#include "utils.h"

namespace audioplayers_linux_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

/// The codec used by AudioPlayersApi.
const flutter::StandardMethodCodec& AudioPlayersApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `AudioPlayersApi` to handle messages through the
// `binary_messenger`.
void AudioPlayersApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                            AudioPlayersApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<EncodableValue>>(
        binary_messenger, "xyz.luan/audioplayers", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler([api](const MethodCall<EncodableValue>&
                                              methodCall,
                                          std::unique_ptr<MethodResult<
                                              EncodableValue>> result) {
        std::cerr << methodCall.method_name() << std::endl;

        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        std::string playerId;
        for (auto& it : *args) {
          if ("playerId" == std::get<std::string>(it.first) &&
              std::holds_alternative<std::string>(it.second)) {
            playerId = std::get<std::string>(it.second);
          }
        }
        std::cerr << "playerId: [" << playerId << "]" << std::endl;
        if (playerId.empty()) {
          result->Error("LinuxAudioError",
                        "Call missing mandatory parameter playerId.",
                        EncodableValue());
          return;
        }

        try {
          if (methodCall.method_name() == "create") {
            api->Create(playerId, [&](std::optional<FlutterError>&& output) {
              if (output.has_value()) {
                result->Error("create", "failed", WrapError(output.value()));
                return;
              }
              result->Success(EncodableValue(1));
              return;
            });
            return;
          }

          auto player = AudioplayersLinuxPlugin::GetPlayer(playerId);
          if (!player) {
            std::cerr << "Player has not yet been created or has already been "
                         "disposed."
                      << std::endl;
            result->Error(
                "LinuxAudioError",
                "Player has not yet been created or has already been disposed.",
                EncodableValue());
            return;
          }

          const auto& method_name = methodCall.method_name();
          std::cerr << method_name << std::endl;
          if (method_name == "pause") {
            player->Pause();
          } else if (method_name == "resume") {
            player->Resume();
          } else if (method_name == "stop") {
            player->Stop();
          } else if (method_name == "release") {
            player->ReleaseMediaSource();
          } else if (method_name == "seek") {
            EncodableValue valuePosition;
            for (auto& it : *args) {
              if ("position" == std::get<std::string>(it.first)) {
                valuePosition = it.second;
                break;
              }
            }
            int32_t set_position =
                valuePosition.IsNull()
                    ? static_cast<int32_t>(player->GetPosition().value_or(0))
                    : std::get<int32_t>(valuePosition);
            player->SetPosition(set_position);
          } else if (method_name == "setSourceUrl") {
            EncodableValue valueUrl;
            EncodableValue valueIsLocal;
            for (auto& it : *args) {
              if ("url" == std::get<std::string>(it.first)) {
                valueUrl = it.second;
              } else if ("isLocal" == std::get<std::string>(it.first))
                valueIsLocal = it.second;
            }

            if (valueUrl.IsNull()) {
              result->Error("LinuxAudioError",
                            "Null URL received on setSourceUrl.",
                            EncodableValue());
              return;
            }
            auto url = std::get<std::string>(valueUrl);
            bool isLocal =
                !valueIsLocal.IsNull() && std::get<bool>(valueIsLocal);
            if (isLocal) {
              url = std::string("file://") + url;
            }
            player->SetSourceUrl(url);
          } else if (method_name == "getDuration") {
            auto optDuration = player->GetDuration();
            result->Success(optDuration.has_value()
                                ? EncodableValue(optDuration.value())
                                : EncodableValue());
            return;
          } else if (method_name == "setVolume") {
            EncodableValue valueVolume;
            for (auto& it : *args) {
              if ("volume" == std::get<std::string>(it.first)) {
                valueVolume = it.second;
                break;
              }
            }
            double volume =
                valueVolume.IsNull() ? 1.0 : std::get<double>(valueVolume);
            player->SetVolume(volume);
          } else if (method_name == "getCurrentPosition") {
            auto optPosition = player->GetPosition();
            result->Success(optPosition.has_value()
                                ? EncodableValue(optPosition.value())
                                : EncodableValue());
            return;
          } else if (method_name == "setPlaybackRate") {
            EncodableValue valuePlaybackRate;
            for (auto& it : *args) {
              if ("playbackRate" == std::get<std::string>(it.first)) {
                valuePlaybackRate = it.second;
                break;
              }
            }
            double playbackRate = valuePlaybackRate.IsNull()
                                      ? 1.0
                                      : std::get<double>(valuePlaybackRate);
            player->SetPlaybackRate(playbackRate);
          } else if (method_name == "setReleaseMode") {
            EncodableValue valueReleaseMode;
            for (auto& it : *args) {
              if ("releaseMode" == std::get<std::string>(it.first)) {
                valueReleaseMode = it.second;
                break;
              }
            }
            std::string releaseMode =
                valueReleaseMode.IsNull()
                    ? std::string()
                    : std::get<std::string>(valueReleaseMode);
            if (releaseMode.empty()) {
              result->Error(
                  "LinuxAudioError",
                  "Error calling setReleaseMode, releaseMode cannot be null",
                  EncodableValue());
              return;
            }
            auto looping = releaseMode.find("loop") != std::string::npos;
            player->SetLooping(looping);
          } else if (method_name == "setPlayerMode") {
            // TODO check support for low latency mode:
            // https://gstreamer.freedesktop.org/documentation/additional/design/latency.html?gi-language=c
          } else if (method_name == "setBalance") {
            EncodableValue valueBalance;
            for (auto& it : *args) {
              if ("balance" == std::get<std::string>(it.first)) {
                valueBalance = it.second;
                break;
              }
            }
            double balance =
                valueBalance.IsNull() ? 0.0f : std::get<double>(valueBalance);
            player->SetBalance(static_cast<float>(balance));
          } else if (method_name == "emitLog") {
            EncodableValue valueMessage;
            for (auto& it : *args) {
              if ("message" == std::get<std::string>(it.first)) {
                valueMessage = it.second;
                break;
              }
            }
            std::string message = valueMessage.IsNull()
                                      ? ""
                                      : std::get<std::string>(valueMessage);
            player->OnLog(message.c_str());
          } else if (method_name == "emitError") {
            EncodableValue valueCode;
            EncodableValue valueMessage;
            for (auto& it : *args) {
              if ("code" == std::get<std::string>(it.first)) {
                valueCode = it.second;
                break;
              } else if ("message" == std::get<std::string>(it.first)) {
                valueMessage = it.second;
                break;
              }
            }

            auto code =
                valueCode.IsNull() ? "" : std::get<std::string>(valueCode);
            auto message = valueMessage.IsNull()
                               ? ""
                               : std::get<std::string>(valueMessage);
            player->OnError(code.c_str(), message.c_str(), nullptr, nullptr);
          } else if (method_name == "dispose") {
            player->Dispose();
            // TODO audioPlayers::erase(playerId);
          } else {
            SPDLOG_DEBUG("Unhandled: {}", method_name);
            result->NotImplemented();
            return;
          }
          result->Success();

        } catch (const gchar* error) {
          result->Error("LinuxAudioError", error, EncodableValue());
        } catch (const std::exception& e) {
          result->Error("LinuxAudioError", e.what(), EncodableValue());
        } catch (...) {
          result->Error("LinuxAudioError", "Unknown AudioPlayersLinux error",
                        EncodableValue());
        }
      });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

EncodableValue AudioPlayersApi::WrapError(std::string_view error_message) {
  return EncodableValue(
      EncodableList{EncodableValue(std::string(error_message)),
                    EncodableValue("Error"), EncodableValue()});
}

EncodableValue AudioPlayersApi::WrapError(const FlutterError& error) {
  return EncodableValue(EncodableList{EncodableValue(error.code()),
                                      EncodableValue(error.message()),
                                      error.details()});
}

/// The codec used by AudioPlayersApi.
const flutter::StandardMethodCodec& AudioPlayersGlobalApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `AudioPlayersGlobalApi` to handle messages through the
// `binary_messenger`.
void AudioPlayersGlobalApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                                  AudioPlayersGlobalApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<>>(
        binary_messenger, "xyz.luan/audioplayers.global", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<EncodableValue>& call,
                std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
            Utils::PrintFlutterEncodableValue("global", *call.arguments());
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

EncodableValue AudioPlayersGlobalApi::WrapError(
    std::string_view error_message) {
  return EncodableValue(
      EncodableList{EncodableValue(std::string(error_message)),
                    EncodableValue("Error"), EncodableValue()});
}

EncodableValue AudioPlayersGlobalApi::WrapError(const FlutterError& error) {
  return EncodableValue(EncodableList{EncodableValue(error.code()),
                                      EncodableValue(error.message()),
                                      error.details()});
}

}  // namespace audioplayers_linux_plugin
