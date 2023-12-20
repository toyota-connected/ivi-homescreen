/*
 * Copyright 2023, the Chromium project authors.  Please see the AUTHORS file
 * for details. All rights reserved. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 * Copyright 2023, Toyota Connected North America
 */

#ifndef FLUTTER_PLUGIN_AUDIO_PLAYERS_PLUGIN_H_
#define FLUTTER_PLUGIN_AUDIO_PLAYERS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.g.h"

namespace plugin_audio_players {

class AudioPlayersPlugin final : public flutter::Plugin,
                                 public AudioPlayersApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  AudioPlayersPlugin();

  ~AudioPlayersPlugin() override;

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

#if 0
  void SetAudioContext(
      const std::string& player_id,
      const flutter::EncodableMap& context,
      std::function<void(std::optional<FlutterError> reply)> result) override;
#endif

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

  // Disallow copy and assign.
  AudioPlayersPlugin(const AudioPlayersPlugin&) = delete;
  AudioPlayersPlugin& operator=(const AudioPlayersPlugin&) = delete;

 private:
};

}  // namespace plugin_audio_players

#endif  // FLUTTER_PLUGIN_AUDIO_PLAYERS_PLUGIN_H_
