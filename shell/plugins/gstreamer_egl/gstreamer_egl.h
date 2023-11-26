#pragma once

#include <shell/platform/embedder/embedder.h>

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

  /**
   * @brief OnInitialize method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnInitialize(const FlutterPlatformMessage* message,
                           void* userdata);

  /**
   * @brief OnDispose method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnDispose(const FlutterPlatformMessage* message, void* userdata);

  /**
   * @brief OnCreate method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnCreate(const FlutterPlatformMessage* message, void* userdata);

  /**
   * @brief OnSetLooping method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnSetLooping(const FlutterPlatformMessage* message,
                           void* userdata);

  /**
   * @brief OnPlay method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPlay(const FlutterPlatformMessage* message, void* userdata);

  /**
   * @brief OnPause method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPause(const FlutterPlatformMessage* message, void* userdata);

  /**
   * @brief OnSetVolume method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnSetVolume(const FlutterPlatformMessage* message,
                          void* userdata);

  /**
   * @brief OnSetPlaybackSpeed method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnSetPlaybackSpeed(const FlutterPlatformMessage* message,
                                 void* userdata);

  /**
   * @brief OnSeekTo method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnSeekTo(const FlutterPlatformMessage* message, void* userdata);

  /**
   * @brief OnPosition method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPosition(const FlutterPlatformMessage* message, void* userdata);

  /**
   * @brief OnSetMixWithOthers method
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnSetMixWithOthers(const FlutterPlatformMessage* message,
                                 void* userdata);
};
