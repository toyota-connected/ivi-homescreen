/*
 * Copyright 2020-2024 Toyota Connected North America
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
#include <flutter/plugin_registrar_homescreen.h>

#include "flutter_desktop_plugin_registrar.h"
#include "messages.g.h"
#include "video_player.h"

namespace video_player_linux {

#define GSTREAMER_DEBUG 1

class VideoPlayerPlugin final : public flutter::Plugin, public VideoPlayerApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarDesktop* registrar);

  explicit VideoPlayerPlugin(flutter::PluginRegistrarDesktop* registrar);

  ~VideoPlayerPlugin() override;

  // Disallow copy and assign.
  VideoPlayerPlugin(const VideoPlayerPlugin&) = delete;
  VideoPlayerPlugin& operator=(const VideoPlayerPlugin&) = delete;

  // VideoPlayerApi methods.
  std::optional<FlutterError> Initialize() override;
  ErrorOr<int64_t> Create(const std::string* asset,
                          const std::string* uri,
                          const flutter::EncodableMap& http_headers) override;
  std::optional<FlutterError> Dispose(int64_t texture_id) override;
  std::optional<FlutterError> SetLooping(int64_t texture_id,
                                         bool is_looping) override;
  std::optional<FlutterError> SetVolume(int64_t texture_id,
                                        double volume) override;
  std::optional<FlutterError> SetPlaybackSpeed(int64_t texture_id,
                                               double speed) override;
  std::optional<FlutterError> Play(int64_t texture_id) override;
  ErrorOr<int64_t> GetPosition(int64_t texture_id) override;
  std::optional<FlutterError> SeekTo(int64_t texture_id,
                                     int64_t position) override;
  std::optional<FlutterError> Pause(int64_t texture_id) override;

 private:
  // A list of all the video players instantiated by this plugin.
  std::map<int64_t, std::unique_ptr<VideoPlayer>> videoPlayers;

  flutter::PluginRegistrarDesktop* registrar_{};

  /**
   * @brief Get video info
   * @param[in] url URL of the stream
   * @param[out] width Buffer to store width info
   * @param[out] height Buffer to store height info
   * @param[out] duration Buffer to store duration info
   * @param[out] codec_id Buffer to store codec info
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * flutter
   */
  static bool get_video_info(const char* url,
                             int& width,
                             int& height,
                             gint64& duration,
                             AVCodecID& codec_id);

  /**
   * @brief Return GStreamer plugin for codec ID
   * @param[in] codec_id Codec ID
   * @return char*
   * @retval GStreamer plugin
   * @relation
   * flutter
   */
  static const char* map_ffmpeg_plugin(AVCodecID codec_id);
};

}  // namespace video_player_linux

#endif  // PLUGINS_VIDEO_PLAYER_LINUX_PLUGIN_H_
