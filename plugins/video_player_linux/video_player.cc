// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "video_player.h"

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/plugin_registrar_homescreen.h>
#include <flutter/standard_method_codec.h>

namespace video_player_linux {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

VideoPlayer::VideoPlayer(FlutterView* view,
                         const std::string* /* uri */,
                         const flutter::EncodableMap& /* httpHeaders */)
    : VideoPlayer(view) {
    //TODO
}

VideoPlayer::VideoPlayer(FlutterView* /* view */)
    : texture(flutter::GpuSurfaceTexture(
          FlutterDesktopGpuSurfaceType::
              kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
          [this](auto&& PH1, auto&& PH2) {
            return ObtainDescriptorCallback(std::forward<decltype(PH1)>(PH1),
                                            std::forward<decltype(PH2)>(PH2));
          })) {
    //TODO
}

void VideoPlayer::Init(flutter::PluginRegistrar* registrar, int64_t textureId) {
  _textureId = textureId;

  _eventChannel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(),
          std::string("flutter.io/videoPlayer/videoEvents") +
              std::to_string(textureId),
          &flutter::StandardMethodCodec::GetInstance());

  _eventChannel->SetStreamHandler(
      std::make_unique<
          flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
          [this](const flutter::EncodableValue* /* arguments */,
                 std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&
                     events)
              -> std::unique_ptr<
                  flutter::StreamHandlerError<flutter::EncodableValue>> {
            this->_eventSink = std::move(events);
            return nullptr;
          },
          [this](const flutter::EncodableValue* /* arguments */)
              -> std::unique_ptr<
                  flutter::StreamHandlerError<flutter::EncodableValue>> {
            this->_eventSink = nullptr;
            return nullptr;
          }));

  _textureRegistry = registrar->texture_registrar();
}

VideoPlayer::~VideoPlayer() {
  m_valid = false;
}

bool VideoPlayer::IsValid() {
  return m_valid;
}

FlutterDesktopGpuSurfaceDescriptor* VideoPlayer::ObtainDescriptorCallback(
    size_t /* width */,
    size_t /* height */) {
  // Lock buffer mutex to protect texture processing
  std::lock_guard<std::mutex> buffer_lock(m_buffer_mutex);
  // TODO
  return &m_descriptor;
}

void VideoPlayer::OnMediaInitialized() {
  // Start playback
  //TODO
  if (!this->isInitialized) {
    this->isInitialized = true;
    this->SendInitialized();
  }
}

void VideoPlayer::UpdateVideoSize() {
  //TODO
}

void VideoPlayer::OnPlaybackEnded() {
  if (this->_eventSink) {
    this->_eventSink->Success(flutter::EncodableValue(
        flutter::EncodableMap({{flutter::EncodableValue("event"),
                                flutter::EncodableValue("completed")}})));
  }
}

void VideoPlayer::SetBuffering(bool buffering) {
  if (_eventSink) {
    _eventSink->Success(flutter::EncodableValue(flutter::EncodableMap(
        {{flutter::EncodableValue("event"),
          flutter::EncodableValue(buffering ? "bufferingStart"
                                            : "bufferingEnd")}})));
  }
}

void VideoPlayer::SendInitialized() {
  if (isInitialized) {
    auto event = EncodableMap(
        {{EncodableValue("event"), EncodableValue("initialized")},
         {
             EncodableValue("duration"),
             EncodableValue((int64_t)0)  // m_mediaEngineWrapper->GetDuration())
         }});

    uint32_t width = 0;
    uint32_t height = 0;
    //    m_mediaEngineWrapper->GetNativeVideoSize(width, height);
    event.insert({EncodableValue("width"), EncodableValue((int32_t)width)});
    event.insert({EncodableValue("height"), EncodableValue((int32_t)height)});

    if (this->_eventSink) {
      _eventSink->Success(flutter::EncodableValue(event));
    }
  }
}

void VideoPlayer::Dispose() {
  if (isInitialized) {
    //TODO
  }
  _eventChannel = nullptr;
}

void VideoPlayer::SetLooping(bool /* isLooping */) {
  //TODO
}

void VideoPlayer::SetVolume(double /* volume */) {
  //TODO
}

void VideoPlayer::SetPlaybackSpeed(double /* playbackSpeed */) {
  //TODO
}

void VideoPlayer::Play() {
  //TODO
}

void VideoPlayer::Pause() {
  //TODO
}

int64_t VideoPlayer::GetPosition() {
  //TODO
  return 0;  // TODO m_mediaEngineWrapper->GetMediaTime();
}

void VideoPlayer::SendBufferingUpdate() {
  auto values = EncodableList();
  //TODO
  if (this->_eventSink) {
    this->_eventSink->Success(EncodableValue(EncodableMap(
        {{EncodableValue("event"), EncodableValue("bufferingUpdate")},
         {EncodableValue("values"), EncodableValue(values)}})));
  }
}

void VideoPlayer::SeekTo(int64_t /* seek */) {
  //TODO
}

int64_t VideoPlayer::GetTextureId() const {
  return _textureId;
}

}  // namespace video_player_linux
