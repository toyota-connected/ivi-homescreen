#pragma once

#include <flutter_embedder.h>

class GstreamerEgl {
 public:
  static constexpr char kChannelGstreamerInitialize[] =
      "dev.flutter.pigeon.VideoPlayerApi.initialize";
  static constexpr char kChannelGstreamerCreate[] =
      "dev.flutter.pigeon.VideoPlayerApi.create";
  static constexpr char kChannelGstreamerDispose[] =
      "dev.flutter.pigeon.VideoPlayerApi.dispose";
  static constexpr char kChannelGstreamerSetLooping[] =
      "dev.flutter.pigeon.VideoPlayerApi.setLooping";
  static constexpr char kChannelGstreamerSetVolume[] =
      "dev.flutter.pigeon.VideoPlayerApi.setVolume";
  static constexpr char kChannelGstreamerSetPlaybackSpeed[] =
      "dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed";
  static constexpr char kChannelGstreamerPlay[] =
      "dev.flutter.pigeon.VideoPlayerApi.play";
  static constexpr char kChannelGstreamerPosition[] =
      "dev.flutter.pigeon.VideoPlayerApi.position";
  static constexpr char kChannelGstreamerSeekTo[] =
      "dev.flutter.pigeon.VideoPlayerApi.seekTo";
  static constexpr char kChannelGstreamerPause[] =
      "dev.flutter.pigeon.VideoPlayerApi.pause";
  static constexpr char kChannelGstreamerSetMixWithOthers[] =
      "dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers";
  static constexpr char kChannelGstreamerEventPrefix[] =
      "flutter.io/videoPlayer/videoEvents";

  static void OnInitialize(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnDispose(const FlutterPlatformMessage* message, void* userdata);
  static void OnCreate(const FlutterPlatformMessage* message, void* userdata);
  static void OnSetLooping(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnPlay(const FlutterPlatformMessage* message, void* userdata);
  static void OnPause(const FlutterPlatformMessage* message, void* userdata);
  static void OnSetVolume(const FlutterPlatformMessage* message,
                          void* userdata);
  static void OnSetPlaybackSpeed(const FlutterPlatformMessage* message,
                                 void* userdata);
  static void OnSeekTo(const FlutterPlatformMessage* message, void* userdata);
  static void OnPosition(const FlutterPlatformMessage* message, void* userdata);
  static void OnSetMixWithOthers(const FlutterPlatformMessage* message,
                                 void* userdata);
};
