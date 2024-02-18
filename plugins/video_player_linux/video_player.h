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

#pragma once

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/plugin_registrar_homescreen.h>
#include <flutter/standard_method_codec.h>

#include "nv12.h"

extern "C" {
#include <gst/gst.h>
#include <gst/video/video.h>
#include <libavformat/avformat.h>
}

#include "messages.g.h"

class Backend;

namespace video_player_linux {

class VideoPlayer {
 public:
  VideoPlayer(flutter::PluginRegistrarDesktop* registrar,
              const std::string& uri,
              std::map<std::string, std::string> http_headers,
              GLsizei width,
              GLsizei height,
              gint64 duration,
              GstElementFactory* decoder_factory);
  ~VideoPlayer();

  void Dispose();
  void SetLooping(bool isLooping);
  void SetVolume(double volume);
  void SetPlaybackSpeed(double playbackSpeed);
  void Play();
  void Pause();
  int64_t GetPosition();
  void SendBufferingUpdate();
  void SeekTo(int64_t seek);
  int64_t GetTextureId() const { return m_texture_id; };
  bool IsValid();

  // Initializes the video player.
  void Init(flutter::BinaryMessenger* messenger,
            flutter::TextureRegistrar* texture_registry);

 private:
  flutter::PluginRegistrarDesktop* m_registrar;
  std::string uri_;
  std::map<std::string, std::string> http_headers_;
  GLsizei width_{};
  GLsizei height_{};
  gint64 duration_{};
  GstElementFactory* decoder_factory_;

  GLuint m_texture_id{};
  std::atomic<bool> m_valid = true;
  std::mutex m_buffer_mutex;
  flutter::TextureRegistrar* m_texture_registry{};
  std::unique_ptr<flutter::GpuSurfaceTexture> gpu_surface_texture_;

  GMainContext* context_;
  GstState media_state_;

  // Gst members
  GstElement* playbin_{};
  GstElement* pipeline_{};
  GstElement* sink_{};
  GstElement* decoder_{};
  GstElement* video_convert_{};
  GstElement* video_scale_{};
  GstCaps* scale_{};
  GstVideoInfo info_{};
  gint64 position_ = 0;
  gdouble rate_ = 0.0;
  GstBus* bus_{};

  gulong handoff_handler_id_;
  gulong on_bus_msg_id_;

  GstState target_state_ = GST_STATE_PAUSED;

  AVCodecID codec_id_{};

  gint n_video_{};
  gint current_video_{};
  std::unique_ptr<nv12::Shader> shader_;
  bool is_looping_{};
  bool is_buffering_{};
  gboolean is_live_{};
  bool events_enabled_{};
  double volume_ = 0.0;

  std::mutex gst_mutex_;

  bool is_initialized_ = false;
  void SetBuffering(bool buffering);

  void OnPlaybackEnded();
  void OnMediaInitialized();
  void OnMediaStateChange(GstState state);
  static void OnMediaError(GstMessage* msg);
  void OnMediaDurationChange();
  void SendInitialized();

  static void OnTag(const GstTagList* list,
                    const gchar* tag,
                    gpointer user_data);

  // The Surface Descriptor sent to Flutter when a texture frame is available.
  FlutterDesktopGpuSurfaceDescriptor m_descriptor{};

  // A mutex is used to synchronize access to the texture descriptor.
  std::mutex buffer_mutex_;

  // The internal Flutter event channel instance.
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;

  // The internal Flutter event sink instance, used to send events to the Dart
  // side.
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;

  /**
   * @brief Creates all items required for Texture
   * @param[in] width texture width
   * @param[in] height texture height
   * @return bool true if successful
   * @relation
   * wayland
   */
  bool EnsureTextureCreated(uint32_t width, uint32_t height);

  /**
   * @brief Callback called when fakesink receives new frame data
   * @param[in] fakesink No use
   * @param[in] buffer Pointer to New frame data
   * @param[in] pad No use
   * @param[in,out] _data Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void handoff_handler(GstElement* fakesink,
                              GstBuffer* buffer,
                              GstPad* pad,
                              void* user_data);

  static gboolean OnBusMessage(GstBus* bus, GstMessage* msg, void* user_data);

  /**
   * @brief Load RGB pixels
   * @param[in] textureId Texture image id
   * @param[in] data Pointer to image data
   * @param[in] width Texture image width
   * @param[in] height Texture image height
   * @return void
   * @relation
   * flutter
   */
  static void load_rgb_pixels(GLuint textureId,
                              const unsigned char* data,
                              const int width,
                              const int height);

  /**
   * @brief Load shaders
   * @param[in] vsource Source code to load into vertexShader
   * @param[in] fsource Source code to load into fragmentShader
   * @return GLuint
   * @retval Non-zero Normal end
   * @retval 0 Abnormal end
   * @relation
   * flutter
   */
  GLuint load_shaders(const GLchar* vsource, const GLchar* fsource);

  /**
   * @brief Prepare
   * @param[in,out] data Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void prepare(VideoPlayer* user_data);
};
}  // namespace video_player_linux
