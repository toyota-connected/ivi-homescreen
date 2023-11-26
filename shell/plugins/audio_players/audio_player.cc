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

#include "audio_player.h"

#include <map>
#include <sstream>

#include <flutter/standard_message_codec.h>

#include <gst/audio/audio.h>

#include "audio_players.h"
#include "logging/logging.h"
#include "platform_channel.h"

#define STR_LINK_TROUBLESHOOTING \
  "https://github.com/bluefireteam/audioplayers/blob/main/troubleshooting.md"

namespace Plugins {

/**
 * @brief Main Loop
 * @param[in,out] data Pointer to User data
 * @return void
 * @relation
 * flutter
 */
void AudioPlayer::main_loop(AudioPlayer* data) {
  SPDLOG_TRACE("({}) main_loop", data->engine_->GetIndex());
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);

  data->playbin_ = gst_element_factory_make("playbin", nullptr);
  if (!data->playbin_) {
    throw std::runtime_error("Not all elements could be created.");
  }
  // Setup stereo balance controller
  data->panorama_ = gst_element_factory_make("audiopanorama", nullptr);
  if (data->panorama_) {
    data->audiobin_ = gst_bin_new(nullptr);
    data->audiosink_ = gst_element_factory_make("autoaudiosink", nullptr);

    gst_bin_add_many(GST_BIN(data->audiobin_), data->panorama_,
                     data->audiosink_, nullptr);
    gst_element_link(data->panorama_, data->audiosink_);

    GstPad* sinkpad = gst_element_get_static_pad(data->panorama_, "sink");
    data->panoramaSinkPad_ = gst_ghost_pad_new("sink", sinkpad);
    gst_element_add_pad(data->audiobin_, data->panoramaSinkPad_);
    gst_object_unref(GST_OBJECT(sinkpad));

    g_object_set(G_OBJECT(data->playbin_), "audio-sink", data->audiobin_,
                 nullptr);
    g_object_set(G_OBJECT(data->panorama_), "method", 1, nullptr);
  }

  // Setup source options
  g_signal_connect(data->playbin_, "source-setup",
                   G_CALLBACK(AudioPlayer::SourceSetup), &data->source_);

  data->bus_ = gst_element_get_bus(data->playbin_);
  GSource* bus_source = gst_bus_create_watch(data->bus_);
  g_source_set_callback(
      bus_source, reinterpret_cast<GSourceFunc>(gst_bus_async_signal_func),
      nullptr, nullptr);
  g_source_attach(bus_source, context);
  g_source_unref(bus_source);
  g_signal_connect(data->bus_, "message", G_CALLBACK(OnBusMessage), data);
  gst_object_unref(data->bus_);

  data->main_loop_ = g_main_loop_new(context, FALSE);
  g_main_loop_run(data->main_loop_);
  g_main_loop_unref(data->main_loop_);
  data->main_loop_ = nullptr;
  SPDLOG_INFO("({}) [main_loop] mainloop end", data->engine_->GetIndex());
}

AudioPlayer::AudioPlayer(std::string playerId, Engine* engine)
    : playerId_(std::move(playerId)), engine_(engine) {
  std::stringstream ss;
  ss << kEventChannelPrefix << playerId_;
  eventChannel_ = ss.str();
  PlatformChannel::GetInstance()->RegisterCallback(eventChannel_.c_str(),
                                                   OnPlatformMessage);

  gthread_ = std::make_unique<std::thread>(main_loop, this);
}

AudioPlayer::~AudioPlayer() {
  gst_element_set_state(playbin_, GST_STATE_NULL);
  g_main_loop_quit(main_loop_);
  g_main_loop_unref(main_loop_);
  gthread_->join();
  gthread_.reset();
}

void AudioPlayer::SourceSetup(GstElement* /* playbin */,
                              GstElement* source,
                              GstElement** /* p_src */) {
  // Allow sources from unencrypted / misconfigured connections
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "ssl-strict") !=
      nullptr) {
    g_object_set(G_OBJECT(source), "ssl-strict", FALSE, NULL);
  }
}

void AudioPlayer::SetSourceUrl(const std::string& url, const bool isLocal) {
  spdlog::debug("[{}] SetSourceUrl: {}", playerId_, url);
  isLocal_ = isLocal;
  if (url_ != url) {
    url_ = url;
    gst_element_set_state(playbin_, GST_STATE_NULL);
    isInitialized_ = false;
    isPlaying_ = false;
    if (!url_.empty()) {
      g_object_set(GST_OBJECT(playbin_), "uri", url_.c_str(), NULL);
      if (playbin_->current_state != GST_STATE_READY) {
        if (gst_element_set_state(playbin_, GST_STATE_READY) ==
            GST_STATE_CHANGE_FAILURE) {
          throw std::runtime_error(
              "Unable to set the pipeline to GST_STATE_READY.");
        }
      }
    }
  } else {
    OnPrepared(true);
  }
}

void AudioPlayer::ReleaseMediaSource() {
  spdlog::debug("[{}] ReleaseMediaSource", playerId_);
  if (isPlaying_)
    isPlaying_ = false;
  if (isInitialized_)
    isInitialized_ = false;
  url_.clear();

  GstState playbinState;
  gst_element_get_state(playbin_, &playbinState, nullptr, GST_CLOCK_TIME_NONE);
  if (playbinState > GST_STATE_NULL) {
    gst_element_set_state(playbin_, GST_STATE_NULL);
  }
}

void AudioPlayer::OnTagItem(const GstTagList* /* list */,
                            const gchar* tag,
                            AudioPlayer* /* data */) {
  spdlog::debug("gst: [tag] {} - {}", tag, gst_tag_get_description(tag));
}

gboolean AudioPlayer::OnBusMessage(GstBus* /* bus */,
                                   GstMessage* message,
                                   AudioPlayer* data) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError* err;
      gchar* debug;

      gst_message_parse_error(message, &err, &debug);
      data->OnMediaError(err, debug);
      g_error_free(err);
      g_free(debug);
      break;
    }
    case GST_MESSAGE_NEW_CLOCK:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->playbin_)) {
        data->OnDurationUpdate();
      }
      break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->playbin_)) {
        GstState old_state, new_state;
        gst_message_parse_state_changed(message, &old_state, &new_state,
                                        nullptr);
        data->playbin_state_ = new_state;
        data->OnMediaStateChange(GST_MESSAGE_SRC(message), &old_state,
                                 &new_state);
      }
      break;
    case GST_MESSAGE_BUFFERING: {
      gst_message_parse_buffering(message, &data->bufferingPercent_);
      break;
    }
    case GST_MESSAGE_EOS:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->playbin_)) {
        if (data->isPlaying_) {
          data->OnPlaybackEnded();
        }
      }
      break;
    case GST_MESSAGE_ASYNC_DONE:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->playbin_)) {
        if (!data->isSeekCompleted_) {
          data->OnSeekCompleted();
          data->isSeekCompleted_ = true;
        }
      }
      break;
    case GST_MESSAGE_TAG: {
      GstTagList* tag_list;
      gst_message_parse_tag(message, &tag_list);
      gst_tag_list_foreach(
          tag_list, reinterpret_cast<GstTagForeachFunc>(OnTagItem), data);
      gst_tag_list_unref(tag_list);
      break;
    }
    default:
      SPDLOG_DEBUG("[{}] gst: [{}] {}", data->playerId_,
                   GST_OBJECT_NAME(message->src),
                   GST_MESSAGE_TYPE_NAME(message));
      break;
  }

  // Continue watching for messages
  return TRUE;
}

void AudioPlayer::OnMediaError(GError* error, gchar* /* debug */) const {
  gchar const* code = "LinuxAudioError";
  gchar const* message;
  std::stringstream detailsStr;
  detailsStr << error->message
             << " (Domain: " << g_quark_to_string(error->domain)
             << ", Code: " << error->code << ")";
  const flutter::EncodableValue details(detailsStr.str().c_str());
  // https://gstreamer.freedesktop.org/documentation/gstreamer/gsterror.html#enumerations
  if (error->domain == GST_STREAM_ERROR) {
    message =
        "Failed to set source. For troubleshooting, "
        "see: " STR_LINK_TROUBLESHOOTING;
  } else {
    message = "Unknown GstGError. See details.";
  }
  OnError(code, message, details, &error);
}

void AudioPlayer::OnError(const gchar* code,
                          const gchar* message,
                          const flutter::EncodableValue& /* details */,
                          GError** /* error */) const {
  auto encoded =
      flutter::StandardMethodCodec::GetInstance().EncodeErrorEnvelope(code,
                                                                      message);
  spdlog::debug("[{}] OnError: ({}) {}", playerId_, code, message);
  engine_->SendPlatformMessage(eventChannel_.c_str(), std::move(encoded));
}

static std::string GetStateName(const GstState* state) {
  switch (*state) {
    case GST_STATE_VOID_PENDING:
      return "GST_STATE_VOID_PENDING";
    case GST_STATE_NULL:
      return "GST_STATE_NULL";
    case GST_STATE_READY:
      return "GST_STATE_READY";
    case GST_STATE_PAUSED:
      return "GST_STATE_PAUSED";
    case GST_STATE_PLAYING:
      return "GST_STATE_PLAYING";
    default:
      return "";
  }
}

void AudioPlayer::OnMediaStateChange(const GstObject* src,
                                     const GstState* old_state,
                                     const GstState* new_state) {
  SPDLOG_DEBUG("[{}] OnMediaStateChange: old: {}, new: {}", playerId_,
               GetStateName(old_state), GetStateName(new_state));

  if (!playbin_) {
    OnError("LinuxAudioError",
            "Player was already disposed (OnMediaStateChange).", nullptr,
            nullptr);
    return;
  }

  if (src == GST_OBJECT(playbin_)) {
    if (*new_state == GST_STATE_READY) {
      // Need to set to pause state, in order to make player functional
      if (gst_element_set_state(playbin_, GST_STATE_PAUSED) ==
          GST_STATE_CHANGE_FAILURE) {
        gchar const* errorDescription =
            "Unable to set the pipeline from GST_STATE_READY to "
            "GST_STATE_PAUSED.";
        if (isInitialized_) {
          OnError("LinuxAudioError", errorDescription, nullptr, nullptr);
        } else {
          OnError("LinuxAudioError",
                  "Failed to set source. For troubleshooting, "
                  "see: " STR_LINK_TROUBLESHOOTING,
                  flutter::EncodableValue(errorDescription), nullptr);
        }
      }
      if (isInitialized_) {
        isInitialized_ = false;
        SPDLOG_DEBUG("[{}] isInitialized_ = false", playerId_);
      }
    } else if (*old_state == GST_STATE_READY &&
               *new_state == GST_STATE_PLAYING) {
      SPDLOG_DEBUG("[{}] QueryDuration", playerId_);
      OnDurationUpdate();
    } else if (*new_state >= GST_STATE_PAUSED) {
      if (!isInitialized_) {
        isInitialized_ = true;
        SPDLOG_DEBUG("[{}] isInitialized_ = true", playerId_);
        OnPrepared(true);
        if (isPlaying_) {
          Resume();
        }
      }
    } else if (isInitialized_) {
      isInitialized_ = false;
    }
  }
}

void AudioPlayer::OnPrepared(bool isPrepared) const {
  const flutter::EncodableMap map(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onPrepared")},
       {flutter::EncodableValue("value"),
        flutter::EncodableValue(isPrepared)}});
  auto encoded =
      flutter::StandardMessageCodec::GetInstance().EncodeMessage(map);
  engine_->SendPlatformMessage(eventChannel_.c_str(), std::move(encoded));
}

void AudioPlayer::OnDurationUpdate() const {
  int64_t duration = GetDuration().value_or(0);
  const flutter::EncodableMap map(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onDuration")},
       {flutter::EncodableValue("value"), flutter::EncodableValue(duration)}});
  auto encoded =
      flutter::StandardMessageCodec::GetInstance().EncodeMessage(map);
  spdlog::debug("[{}] audio.onDuration {}", playerId_, duration);
  engine_->SendPlatformMessage(eventChannel_.c_str(), std::move(encoded));
}

void AudioPlayer::OnSeekCompleted() const {
  const flutter::EncodableMap map(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onSeekComplete")},
       {flutter::EncodableValue("value"), flutter::EncodableValue(true)}});
  auto encoded =
      flutter::StandardMessageCodec::GetInstance().EncodeMessage(map);
  spdlog::debug("[{}] audio.onSeekComplete", playerId_);
  engine_->SendPlatformMessage(eventChannel_.c_str(), std::move(encoded));
}

void AudioPlayer::OnPlaybackEnded() {
  if (GetLooping()) {
    Play();
  } else {
    Pause();
    SetPosition(0);
  }
  const flutter::EncodableMap map(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onComplete")},
       {flutter::EncodableValue("value"), flutter::EncodableValue(true)}});
  auto encoded =
      flutter::StandardMessageCodec::GetInstance().EncodeMessage(map);
  spdlog::debug("[{}] audio.onComplete", playerId_);
  engine_->SendPlatformMessage(eventChannel_.c_str(), std::move(encoded));
}

void AudioPlayer::OnLog(const gchar* message) const {
  const flutter::EncodableMap map(
      {{flutter::EncodableValue("event"),
        flutter::EncodableValue("audio.onLog")},
       {flutter::EncodableValue("value"),
        flutter::EncodableValue(std::string(message))}});
  auto encoded =
      flutter::StandardMessageCodec::GetInstance().EncodeMessage(map);
  spdlog::debug("[{}] audio.onLog: {}", playerId_, message);
  engine_->SendPlatformMessage(eventChannel_.c_str(), std::move(encoded));
}

void AudioPlayer::SetReleaseMode(std::string mode) {
  spdlog::debug("[{}] SetReleaseMode {}", playerId_, mode);
  releaseMode_ = std::move(mode);
}

void AudioPlayer::SetPlayerMode(std::string playerMode) {
  spdlog::debug("[{}] SetPlayerMode {}", playerId_, playerMode);
  playerMode_ = std::move(playerMode);
}

void AudioPlayer::SetBalance(float balance) const {
  spdlog::debug("[{}] SetBalance {}", playerId_, balance);

  if (!panorama_) {
    OnLog("Audiopanorama was not initialized");
    return;
  }

  if (balance > 1.0f) {
    balance = 1.0f;
  } else if (balance < -1.0f) {
    balance = -1.0f;
  }
  g_object_set(G_OBJECT(panorama_), "panorama", balance, NULL);
}

void AudioPlayer::SetLooping(const bool isLooping) {
  spdlog::debug("[{}] SetLooping {}", playerId_, isLooping);
  isLooping_ = isLooping;
}

bool AudioPlayer::GetLooping() const {
  spdlog::debug("[{}] GetLooping {}", playerId_, isLooping_);
  return isLooping_;
}

void AudioPlayer::SetVolume(const double volume) const {
  spdlog::debug("[{}] SetVolume", playerId_);

  double value = volume;
  if (volume > 1) {
    value = 1;
  } else if (volume < 0) {
    value = 0;
  }
  g_object_set(G_OBJECT(playbin_), "volume", value, NULL);
}

/**
 * A rate of 1.0 means normal playback rate, 2.0 means double speed.
 * Negatives values means backwards playback.
 * A value of 0.0 will pause the player.
 *
 * @param position the position in milliseconds
 * @param rate the playback rate (speed)
 */
void AudioPlayer::SetPlayback(const int64_t position, const double rate) {
  spdlog::debug("[{}] SetPlayback", playerId_);

  if (rate != 0 && playbackRate_ != rate) {
    playbackRate_ = rate;
  }

  if (!isInitialized_) {
    return;
  }
  if (!isSeekCompleted_) {
    return;
  }
  if (rate == 0) {
    // Do not set rate if it's 0, rather pause.
    Pause();
    return;
  }

  isSeekCompleted_ = false;

  GstEvent* seek_event;
  if (rate > 0) {
    seek_event = gst_event_new_seek(
        rate, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, position * GST_MSECOND, GST_SEEK_TYPE_NONE, -1);
  } else {
    seek_event = gst_event_new_seek(
        rate, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, position * GST_MSECOND);
  }

  if (!gst_element_send_event(playbin_, seek_event)) {
    std::stringstream ss;
    ss << "Could not set playback to position " << position << " and rate "
       << rate;
    OnLog(ss.str().c_str());
    isSeekCompleted_ = true;
  }
}

void AudioPlayer::SetPlaybackRate(const double rate) {
  spdlog::debug("[{}] SetPlaybackRate", playerId_);
  SetPlayback(GetPosition().value_or(0), rate);
}

/**
 * @param position the position in milliseconds
 */
void AudioPlayer::SetPosition(const int64_t position) {
  spdlog::debug("[{}] SetPosition", playerId_);
  if (!isInitialized_) {
    return;
  }
  SetPlayback(position, playbackRate_);
}

/**
 * @return int64_t the position in milliseconds
 */
std::optional<int64_t> AudioPlayer::GetPosition() const {
  gint64 current = 0;
  if (playbin_state_ >= GST_STATE_READY) {
    if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &current)) {
      OnLog("Could not query current position.");
      return std::nullopt;
    }
    spdlog::debug("[{}] GetPosition: {}", current);
    return std::make_optional(current / 1000000);
  }
  return std::nullopt;
}

/**
 * @return int64_t the duration in milliseconds
 */
std::optional<int64_t> AudioPlayer::GetDuration() const {
  gint64 duration = 0;
  if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration)) {
    // FIXME: Get duration for MP3 with variable bit rate with gst-discoverer:
    // https://gstreamer.freedesktop.org/documentation/pbutils/gstdiscoverer.html?gi-language=c#gst_discoverer_info_get_duration
    OnLog("Could not query current duration");
    return std::nullopt;
  }
  return std::make_optional(duration / 1000000);
}

void AudioPlayer::Play() {
  spdlog::debug("[{}] Play", playerId_);
  SetPosition(0);
  Resume();
}

void AudioPlayer::Pause() {
  spdlog::debug("[{}] Pause", playerId_);
  if (isPlaying_) {
    isPlaying_ = false;
  }
  if (!isInitialized_) {
    return;
  }
  if (GST_STATE_CHANGE_SUCCESS ==
      gst_element_set_state(playbin_, GST_STATE_PAUSED)) {
  } else {
    throw std::runtime_error("Unable to set the pipeline to GST_STATE_PAUSED.");
  }
}

void AudioPlayer::Resume() {
  spdlog::debug("[{}] Resume", playerId_);
  if (!isPlaying_) {
    isPlaying_ = true;
  }
  if (!isInitialized_) {
    return;
  }
  if (GST_STATE_CHANGE_SUCCESS ==
      gst_element_set_state(playbin_, GST_STATE_PLAYING)) {
    // Update duration when start playing, as no event is emitted elsewhere
    OnDurationUpdate();
  } else {
    throw std::runtime_error(
        "Unable to set the pipeline to GST_STATE_PLAYING.");
  }
}

void AudioPlayer::Dispose() {
  if (!playbin_)
    throw std::runtime_error("Player was already disposed (Dispose)");
  ReleaseMediaSource();

  if (bus_) {
    gst_bus_remove_watch(bus_);
    gst_object_unref(GST_OBJECT(bus_));
    bus_ = nullptr;
  }

  if (source_) {
    gst_object_unref(GST_OBJECT(source_));
    source_ = nullptr;
  }

  if (panorama_) {
    gst_element_set_state(audiobin_, GST_STATE_NULL);

    gst_element_remove_pad(audiobin_, panoramaSinkPad_);
    gst_bin_remove(GST_BIN(audiobin_), audiosink_);
    gst_bin_remove(GST_BIN(audiobin_), panorama_);

    // audiobin gets unreferenced (2x) via playbin
    panorama_ = nullptr;
  }

  gst_object_unref(GST_OBJECT(playbin_));
  eventChannel_.clear();
  playbin_ = nullptr;

  g_main_loop_quit(main_loop_);
  g_main_loop_unref(main_loop_);
  gthread_->join();
  gthread_.reset();
}

void AudioPlayer::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  const auto method = obj->method_name();

  if (kMethodListen == method) {
    SPDLOG_TRACE("[audio_player] {}: listen", message->channel);
    result = codec.EncodeSuccessEnvelope();
  } else {
    const auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    Utils::PrintFlutterEncodableValue(method.c_str(), *args);
    result =
        codec.EncodeErrorEnvelope("unimplemented", "method not implemented");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
}  // namespace Plugins