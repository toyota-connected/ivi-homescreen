#pragma once

#include <flutter_embedder.h>

constexpr char kChannelGstreamerInitialize[] =
    "dev.flutter.pigeon.VideoPlayerApi.initialize";
constexpr char kChannelGstreamerCreate[] =
    "dev.flutter.pigeon.VideoPlayerApi.create";
constexpr char kChannelGstreamerDispose[] =
    "dev.flutter.pigeon.VideoPlayerApi.dispose";
constexpr char kChannelGstreamerSetLooping[] =
    "dev.flutter.pigeon.VideoPlayerApi.setLooping";
constexpr char kChannelGstreamerSetVolume[] =
    "dev.flutter.pigeon.VideoPlayerApi.setVolume";
constexpr char kChannelGstreamerSetPlaybackSpeed[] =
    "dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed";
constexpr char kChannelGstreamerPlay[] =
    "dev.flutter.pigeon.VideoPlayerApi.play";
constexpr char kChannelGstreamerPosition[] =
    "dev.flutter.pigeon.VideoPlayerApi.position";
constexpr char kChannelGstreamerSeekTo[] =
    "dev.flutter.pigeon.VideoPlayerApi.seekTo";
constexpr char kChannelGstreamerPause[] =
    "dev.flutter.pigeon.VideoPlayerApi.pause";
constexpr char kChannelGstreamerSetMixWithOthers[] =
    "dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers";
constexpr char kChannelGstreamerEventPrefix[] =
    "flutter.io/videoPlayer/videoEvents";


class Gstreamer {
 public:
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
