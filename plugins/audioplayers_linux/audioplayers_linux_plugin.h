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

#ifndef FLUTTER_PLUGIN_AUDIO_PLAYERS_PLUGIN_H_
#define FLUTTER_PLUGIN_AUDIO_PLAYERS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "audio_player.h"
#include "messages.h"

namespace audioplayers_linux_plugin {

class AudioplayersLinuxPlugin final : public flutter::Plugin,
                                      public AudioPlayersApi,
                                      public AudioPlayersGlobalApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  AudioplayersLinuxPlugin(flutter::BinaryMessenger* messenger);

  ~AudioplayersLinuxPlugin() override;

  void Create(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void Dispose(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void GetCurrentPosition(
      const std::string& player_id,
      std::function<void(ErrorOr<std::optional<int64_t>> reply)> result)
      override;
  void GetDuration(const std::string& player_id,
                   std::function<void(ErrorOr<std::optional<int64_t>> reply)>
                       result) override;
  void Pause(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void Release(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void Resume(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void Seek(
      const std::string& player_id,
      int64_t position,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetBalance(
      const std::string& player_id,
      double balance,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetPlayerMode(
      const std::string& player_id,
      const std::string& player_mode,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetPlaybackRate(
      const std::string& player_id,
      double playback_rate,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetReleaseMode(
      const std::string& player_id,
      const std::string& release_mode,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetSourceBytes(
      const std::string& player_id,
      const std::vector<uint8_t>& bytes,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetSourceUrl(
      const std::string& player_id,
      const std::string& url,
      bool is_local,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetVolume(
      const std::string& player_id,
      double volume,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void Stop(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void EmitLog(
      const std::string& player_id,
      const std::string& message,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void EmitError(
      const std::string& player_id,
      const std::string& code,
      const std::string& message,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void SetAudioContextGlobal(
      const std::string& player_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void EmitLogGlobal(
      const std::string& player_id,
      const std::string& message,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void EmitErrorGlobal(
      const std::string& player_id,
      const std::string& message,
      const std::string& code,
      std::function<void(std::optional<FlutterError> reply)> result) override;

  static AudioPlayer* GetPlayer(const std::string& playerId);

  // Disallow copy and assign.
  AudioplayersLinuxPlugin(const AudioplayersLinuxPlugin&) = delete;
  AudioplayersLinuxPlugin& operator=(const AudioplayersLinuxPlugin&) = delete;

 private:
  flutter::BinaryMessenger* messenger_;

  // void Dispose(GObject* object);

  // void Init(AudioplayersLinuxPlugin* self);
};

}  // namespace audioplayers_linux_plugin

#endif  // FLUTTER_PLUGIN_AUDIO_PLAYERS_PLUGIN_H_
