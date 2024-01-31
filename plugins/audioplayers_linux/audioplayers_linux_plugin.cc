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

#include "audioplayers_linux_plugin.h"

#include <memory>
#include <string>
#include <vector>

#include <flutter/basic_message_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.h"
#include "plugins/common/glib/main_loop.h"

namespace audioplayers_linux_plugin {

static std::map<std::string, std::unique_ptr<AudioPlayer>> audioPlayers_;

// static
void AudioplayersLinuxPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin =
      std::make_unique<AudioplayersLinuxPlugin>(registrar->messenger());

  AudioPlayersApi::SetUp(registrar->messenger(), plugin.get());
  AudioPlayersGlobalApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

AudioplayersLinuxPlugin::AudioplayersLinuxPlugin(
    flutter::BinaryMessenger* messenger)
    : messenger_(messenger) {
  audioPlayers_.clear();

  // GStreamer lib only needs to be initialized once.  Calling it multiple times
  // is fine.
  gst_init(nullptr, nullptr);

  // start the main loop if not already running
  plugin_common::glib::MainLoop::GetInstance();
}

AudioplayersLinuxPlugin::~AudioplayersLinuxPlugin() = default;

AudioPlayer* AudioplayersLinuxPlugin::GetPlayer(const std::string& playerId) {
  auto searchPlayer = audioPlayers_.find(playerId);
  if (searchPlayer == audioPlayers_.end()) {
    return nullptr;
  }
  return searchPlayer->second.get();
}

void AudioplayersLinuxPlugin::Create(
    const std::string& player_id,
    std::function<void(std::optional<FlutterError> reply)> result) {
  auto searchPlayer = audioPlayers_.find(player_id);
  if (searchPlayer == audioPlayers_.end()) {
    std::string event_channel = "xyz.luan/audioplayers/events/" + player_id;
    auto player =
        std::make_unique<AudioPlayer>(std::move(event_channel), messenger_);
    audioPlayers_.insert(
        std::make_pair(player_id, std::move(player)));
  }
  result(std::nullopt);
}

void AudioplayersLinuxPlugin::Dispose(
    const std::string& /* player_id */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::GetCurrentPosition(
    const std::string& /* player_id */,
    std::function<void(ErrorOr<std::optional<int64_t>> reply)> /* result */) {}

void AudioplayersLinuxPlugin::GetDuration(
    const std::string& /* player_id */,
    std::function<void(ErrorOr<std::optional<int64_t>> reply)> /* result */) {}

void AudioplayersLinuxPlugin::Pause(
    const std::string& /* player_id */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::Release(
    const std::string& /* player_id */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::Resume(
    const std::string& /* player_id */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::Seek(
    const std::string& /* player_id */,
    int64_t /* position */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetBalance(
    const std::string& /* player_id */,
    double /* balance */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetPlayerMode(
    const std::string& /* player_id */,
    const std::string& /* player_mode */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetPlaybackRate(
    const std::string& /* player_id */,
    double /* playback_rate */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetReleaseMode(
    const std::string& /* player_id */,
    const std::string& /* release_mode */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetSourceBytes(
    const std::string& /* player_id */,
    const std::vector<uint8_t>& /* bytes */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetSourceUrl(
    const std::string& /* player_id */,
    const std::string& /* url */,
    bool /* is_local */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetVolume(
    const std::string& /* player_id */,
    double /* volume */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::Stop(
    const std::string& /* player_id */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::EmitLog(
    const std::string& /* player_id */,
    const std::string& /* message */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::EmitError(
    const std::string& /* player_id */,
    const std::string& /* code */,
    const std::string& /* message */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::SetAudioContextGlobal(
    const std::string& /* player_id */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::EmitLogGlobal(
    const std::string& /* player_id */,
    const std::string& /* message */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

void AudioplayersLinuxPlugin::EmitErrorGlobal(
    const std::string& /* player_id */,
    const std::string& /* message */,
    const std::string& /* code */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {}

}  // namespace audioplayers_linux_plugin
