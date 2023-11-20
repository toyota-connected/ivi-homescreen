/*
 * Copyright 2017 Luan Nico
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

#include <optional>
#include <string>
#include <thread>

extern "C" {
#include <gst/gst.h>
}

#include "engine.h"

class AudioPlayer {
 public:
  explicit AudioPlayer(std::string playerId, Engine* engine);

  ~AudioPlayer();

  std::optional<int64_t> GetPosition();

  std::optional<int64_t> GetDuration();

  [[nodiscard]] bool GetLooping() const;

  void Play();

  void Pause();

  void Resume();

  void Dispose();

  void SetBalance(float balance);

  void SetLooping(bool isLooping);

  void SetVolume(double volume);

  void SetPlaybackRate(double rate);

  void SetPosition(int64_t position);

  void SetSourceUrl(const std::string& url);

  void ReleaseMediaSource();

  void OnError(const gchar* code,
               const gchar* message,
               const flutter::EncodableValue& details,
               GError** error);

  void OnLog(const gchar* message);

  void SetReleaseMode(std::string mode);

  void SetPlayerMode(std::string playerMode);

 private:
  static constexpr char kEventChannelPrefix[] = "xyz.luan/audioplayers/events/";

  GMainLoop* main_loop_{};
  GstBus* bus_{};
  GstElement* playbin_{};
  GstElement* source_{};
  GstElement* panorama_{};
  GstElement* audiobin_{};
  GstElement* audiosink_{};
  GstPad* panoramaSinkPad_{};

  bool isInitialized_{};
  bool isPlaying_{};
  bool isLooping_{};
  bool isSeekCompleted_ = true;
  double playbackRate_ = 1.0;

  std::unique_ptr<std::thread> gthread_;
  std::string playerId_;
  std::string url_;
  std::string eventChannel_;
  std::string releaseMode_;
  std::string playerMode_;

  Engine* engine_{};

  static void main_loop(AudioPlayer* data);

  static void SourceSetup(GstElement* playbin,
                          GstElement* source,
                          GstElement** p_src);

  static gboolean OnBusMessage(GstBus* bus,
                               GstMessage* message,
                               AudioPlayer* data);

  void SetPlayback(int64_t position, double rate);

  void OnMediaError(GError* error, gchar* debug);

  void OnMediaStateChange(const GstObject* src,
                          const GstState* old_state,
                          const GstState* new_state);

  void OnDurationUpdate();

  void OnSeekCompleted();

  void OnPlaybackEnded();

  void OnPrepared(bool isPrepared);

  /**
   * @brief Callback function for platform messages about isolate
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);
};
