/*
 * Copyright 2020-2023 Toyota Connected North America
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

#ifndef PLUGINS_VIDEO_PLAYER_LINUX_PLUGIN_H_
#define PLUGINS_VIDEO_PLAYER_LINUX_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "flutter_desktop_plugin_registrar.h"
#include "messages.g.h"
#include "video_player.h"

namespace video_player_linux {

class VideoPlayerPlugin final : public flutter::Plugin, public VideoPlayerApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  VideoPlayerPlugin();

  ~VideoPlayerPlugin() override;

  explicit VideoPlayerPlugin(flutter::PluginRegistrar* registrar);

  // Disallow copy and assign.
  VideoPlayerPlugin(const VideoPlayerPlugin&) = delete;
  VideoPlayerPlugin& operator=(const VideoPlayerPlugin&) = delete;

  // VideoPlayerApi methods.
  std::optional<FlutterError> Initialize() override;
  ErrorOr<int64_t> Create(const std::string* asset,
                          const std::string* uri,
                          const std::string* package_name,
                          const std::string* format_hint,
                          const flutter::EncodableMap& http_headers) override;
  std::optional<FlutterError> Dispose(int64_t texture_id) override;
  std::optional<FlutterError> SetLooping(int64_t texture_id,
                                         bool is_looping) override;
  std::optional<FlutterError> SetVolume(int64_t texture_id,
                                        double volume) override;
  std::optional<FlutterError> SetPlaybackSpeed(int64_t texture_id,
                                               double speed) override;
  std::optional<FlutterError> Play(int64_t texture_id) override;
  ErrorOr<int64_t> Position(int64_t texture_id) override;
  std::optional<FlutterError> SeekTo(int64_t texture_id,
                                     int64_t position) override;
  std::optional<FlutterError> Pause(int64_t texture_id) override;
  std::optional<FlutterError> SetMixWithOthers(bool mix_with_others) override;

 private:
  std::map<int64_t, std::unique_ptr<VideoPlayer>> videoPlayers;
  bool mixWithOthers{};

  flutter::TextureRegistrar* _textureRegistry{};
  flutter::PluginRegistrar* registrar_{};
};

}  // namespace video_player_linux

#endif  // PLUGINS_VIDEO_PLAYER_LINUX_PLUGIN_H_
