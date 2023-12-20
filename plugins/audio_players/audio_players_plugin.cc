// Copyright 2023, the Chromium project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
// Copyright 2023, Toyota Connected North America

#include "audio_players_plugin.h"

#include "messages.g.h"

#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <string>
#include <vector>

namespace plugin_audio_players {

// static
void AudioPlayersPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<AudioPlayersPlugin>();

  AudioPlayersApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

AudioPlayersPlugin::AudioPlayersPlugin() = default;

AudioPlayersPlugin::~AudioPlayersPlugin() = default;

void AudioPlayersPlugin::Create(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::Dispose(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::GetCurrentPosition(
    const std::string& player_id,
    std::function<void(ErrorOr<std::optional<int64_t>> reply)> result) {}

void AudioPlayersPlugin::GetDuration(
    const std::string& player_id,
    std::function<void(ErrorOr<std::optional<int64_t>> reply)> result) {}

void AudioPlayersPlugin::Pause(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::Release(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::Resume(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::Seek(
    const std::string& player_id,
    int64_t position,
    std::function<void(std::optional<FlutterError> reply)> result) {}

#if 0
void AudioPlayersPlugin::SetAudioContext(
    const std::string& player_id,
    const flutter::EncodableMap& context,
    std::function<void(std::optional<FlutterError> reply)> result) {}
#endif

void AudioPlayersPlugin::SetBalance(
    const std::string& player_id,
    double balance,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::SetPlayerMode(
    const std::string& player_id,
    const std::string& player_mode,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::SetPlaybackRate(
    const std::string& player_id,
    double playback_rate,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::SetReleaseMode(
    const std::string& player_id,
    const std::string& release_mode,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::SetSourceBytes(
    const std::string& player_id,
    const std::vector<uint8_t>& bytes,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::SetSourceUrl(
    const std::string& player_id,
    const std::string& url,
    bool is_local,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::SetVolume(
    const std::string& player_id,
    double volume,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::Stop(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::EmitLog(
    const std::string& player_id,
    const std::string& message,
    std::function<void(std::optional<FlutterError> reply)> result) {}

void AudioPlayersPlugin::EmitError(
    const std::string& player_id,
    const std::string& code,
    const std::string& message,
    std::function<void(std::optional<FlutterError> reply)> result) {}

}  // namespace plugin_audio_players
