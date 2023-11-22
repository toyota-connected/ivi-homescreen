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
#include <memory>
#include <string>

#include <shell/platform/embedder/embedder.h>

extern "C" {
#include <gst/gst.h>
}

class AudioPlayer;
class AudioPlayers;
class Engine;

class AudioPlayersRegistry {
 public:
  AudioPlayersRegistry();
  ~AudioPlayersRegistry();

  void AddAudioPlayer(const std::string& id,
                      std::unique_ptr<AudioPlayer> audio_player);

  AudioPlayer* GetAudioPlayer(const std::string& id);

  void RemoveAudioPlayer(const std::string& id);

  /**
   * @brief Get instance of AudioPlayersContext class
   * @return AudioPlayersContext&
   * @retval Instance of the AudioPlayersContext class
   * @relation
   * internal
   */
  static AudioPlayersRegistry& GetInstance() {
    if (!sInstance) {
      sInstance = std::make_shared<AudioPlayersRegistry>();
    }
    return *sInstance;
  }

  AudioPlayersRegistry(AudioPlayersRegistry& other) = delete;

  void operator=(const AudioPlayersRegistry&) = delete;

 private:
  static constexpr char kGlobalChannel[] = "xyz.luan/audioplayers.global";
  static constexpr char kEventChannel[] = "xyz.luan/audioplayers.global/events";

  static constexpr char kMethodListen[] = "listen";
  static constexpr char kMethodSetAudioContext[] = "setAudioContext";
  static constexpr char kMethodEmitLog[] = "emitLog";
  static constexpr char kMethodEmitError[] = "emitError";

  static constexpr char kKeyMessage[] = "message";
  static constexpr char kKeyCode[] = "code";

  Engine* engine_{};

  static void OnPlatformMessageGlobal(const FlutterPlatformMessage* message,
                                      void* userdata);
  static void OnPlatformMessageEvents(const FlutterPlatformMessage* message,
                                      void* userdata);

  static void on_global_log(const Engine* engine, const gchar* message);
  static void on_global_error(const Engine* engine,
                              const std::string& code,
                              const std::string& message);

  std::map<std::string, std::unique_ptr<AudioPlayer>> registry_;

 protected:
  static std::shared_ptr<AudioPlayersRegistry> sInstance;
};
