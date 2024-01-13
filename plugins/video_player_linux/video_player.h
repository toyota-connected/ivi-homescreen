// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/plugin_registrar_homescreen.h>
#include <flutter/standard_method_codec.h>

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "messages.g.h"

#include "shell/view/flutter_view.h"

class FlutterView;

namespace video_player_linux {

class VideoPlayer {
 public:
  VideoPlayer(FlutterView* view,
              const std::string* uri,
              const flutter::EncodableMap& httpHeaders);

  void Dispose();
  void SetLooping(bool isLooping);
  void SetVolume(double volume);
  void SetPlaybackSpeed(double playbackSpeed);
  void Play();
  void Pause();
  int64_t GetPosition();
  void SendBufferingUpdate();
  void SeekTo(int64_t seek);
  int64_t GetTextureId() const;
  bool IsValid();

  FlutterDesktopGpuSurfaceDescriptor* ObtainDescriptorCallback(size_t width,
                                                               size_t height);

  void Init(flutter::PluginRegistrar* registrar, int64_t textureId);

  virtual ~VideoPlayer();

  flutter::TextureVariant texture;

 private:
  explicit VideoPlayer(FlutterView* view);

  std::atomic<bool> m_valid = true;
  int64_t _textureId{};

  FlutterDesktopGpuSurfaceDescriptor m_descriptor{};
  std::mutex m_buffer_mutex;
  flutter::TextureRegistrar* _textureRegistry{};

  bool isInitialized = false;

  void SendInitialized();
  void SetBuffering(bool buffering);

  void OnMediaInitialized();
  void OnPlaybackEnded();
  void UpdateVideoSize();

  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> _eventChannel;

  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> _eventSink;
};
}  // namespace video_player_linux
