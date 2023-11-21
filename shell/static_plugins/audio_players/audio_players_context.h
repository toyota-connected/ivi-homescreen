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

#include <map>

extern "C" {
#include <gst/gst.h>
}

class AudioPlayer;
class AudioPlayers;
class Engine;

class AudioPlayersContext {
 public:
  AudioPlayersContext();
  ~AudioPlayersContext();

  void AddAudioPlayer(std::string id,
                      std::unique_ptr<AudioPlayer> audio_player);

  AudioPlayer* GetAudioPlayer(std::string id);

  void RemoveAudioPlayer(std::string id);

  AudioPlayersContext(AudioPlayersContext& other) = delete;

  void operator=(const AudioPlayersContext&) = delete;

 private:
  static constexpr char kGlobalChannel[] = "xyz.luan/audioplayers.global";
  static constexpr char kEventChannel[] = "xyz.luan/audioplayers.global/events";

  Engine *engine_;

  static void OnPlatformMessageGlobal(const FlutterPlatformMessage* message,
                                      void* userdata);
  static void OnPlatformMessageEvents(const FlutterPlatformMessage* message,
                                      void* userdata);

  static void on_global_log(Engine* engine, const gchar* message);

  std::map<std::string, std::unique_ptr<AudioPlayer>> registry_;
};
