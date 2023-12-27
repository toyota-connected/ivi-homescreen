#pragma once

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <flutter/basic_message_channel.h>
#include <flutter/encodable_value.h>
#include <flutter/event_channel.h>
#include <flutter/method_channel.h>

extern "C" {
#include <gst/gst.h>
}

using namespace flutter;

class AudioPlayer : public flutter::BasicMessageChannel<> {
 public:
  AudioPlayer(const std::string& playerId, BinaryMessenger* messenger);

  std::optional<int64_t> GetPosition();

  std::optional<int64_t> GetDuration();

  bool GetLooping() const;

  void Play();

  void Pause();

  void Stop();

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
               flutter::EncodableValue* details,
               GError** error);

  void OnLog(const gchar* message);

  virtual ~AudioPlayer();

 private:
  const std::string eventChannelName_;

  // Gst members
  GMainLoop* main_loop_{};
  GstElement* playbin_{};
  GstElement* source_{};
  GstElement* panorama_{};
  GstElement* audiobin_{};
  GstElement* audiosink_{};
  GstPad* panoramaSinkPad_{};
  GstBus* bus_{};

  bool isInitialized_{};
  bool isPlaying_{};
  bool isLooping_{};
  bool isSeekCompleted_ = true;
  double playbackRate_ = 1.0;

  std::string url_;

  static void SourceSetup(GstElement* playbin,
                          GstElement* source,
                          GstElement** p_src);

  static gboolean OnBusMessage(GstBus* bus,
                               GstMessage* message,
                               AudioPlayer* data);

  void SetPlayback(int64_t seekTo, double rate);

  void OnMediaError(GError* error, gchar* debug);

  void OnMediaStateChange(GstObject* src,
                          const GstState* old_state,
                          const GstState* new_state);

  void OnDurationUpdate();

  void OnSeekCompleted();

  void OnPlaybackEnded();

  void OnPrepared(bool isPrepared);
};
