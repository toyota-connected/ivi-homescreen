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

#include <flutter/encodable_value.h>
#include <shell/platform/embedder/embedder.h>

#include <optional>
#include <string>
#include <thread>

extern "C" {
#include <gst/gst.h>
}

#include "engine.h"

namespace Plugins {

class AudioPlayer {
 public:
  explicit AudioPlayer(std::string playerId, Engine* engine);

  ~AudioPlayer();

  [[nodiscard]] std::optional<int64_t> GetPosition() const;

  [[nodiscard]] std::optional<int64_t> GetDuration() const;

  [[nodiscard]] bool GetLooping() const;

  void Play();

  void Pause();

  void Resume();

  void Dispose();

  void SetBalance(float balance) const;

  void SetLooping(bool isLooping);

  void SetVolume(double volume) const;

  void SetPlaybackRate(double rate);

  void SetPosition(int64_t position);

  void SetSourceUrl(const std::string& url, bool isLocal);

  void ReleaseMediaSource();

  void OnError(const gchar* code,
               const gchar* message,
               const flutter::EncodableValue& details,
               GError** error) const;

  void OnLog(const gchar* message) const;

  void SetReleaseMode(std::string mode);

  void SetPlayerMode(std::string playerMode);

 private:
  static constexpr char kEventChannelPrefix[] = "xyz.luan/audioplayers/events/";

  static constexpr char kMethodListen[] = "listen";

  GMainLoop* main_loop_{};
  GstBus* bus_{};
  GstElement* playbin_{};
  GstElement* source_{};
  GstElement* panorama_{};
  GstElement* audiobin_{};
  GstElement* audiosink_{};
  GstPad* panoramaSinkPad_{};
  GstState playbin_state_{};

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
  bool isLocal_{};
  gint bufferingPercent_ = 0;

  Engine* engine_{};

  static void main_loop(AudioPlayer* data);

  static void SourceSetup(GstElement* playbin,
                          GstElement* source,
                          GstElement** p_src);

  static gboolean OnBusMessage(GstBus* bus,
                               GstMessage* message,
                               AudioPlayer* data);

  static void OnTagItem(const GstTagList* list,
                        const gchar* tag,
                        AudioPlayer* data);

  void SetPlayback(int64_t position, double rate);

  void OnMediaError(GError* error, gchar* debug) const;

  void OnMediaStateChange(const GstObject* src,
                          const GstState* old_state,
                          const GstState* new_state);

  void OnDurationUpdate() const;

  void OnSeekCompleted() const;

  void OnPlaybackEnded();

  void OnPrepared(bool isPrepared) const;

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

}  // namespace AudioPlayers
