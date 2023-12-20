import 'package:pigeon/pigeon.dart';

@ConfigurePigeon(
  PigeonOptions(
    dartOut: 'lib/messages.g.dart',
    // We export in the lib folder to expose the class to other packages.
    dartTestOut: 'lib/test_api.dart',
    cppHeaderOut: 'messages.g.h',
    cppSourceOut: 'messages.g.cc',
    cppOptions: CppOptions(namespace: 'plugin_audio_players'),
    copyrightHeader: 'pigeons/copyright.txt',
  ),
)

@HostApi(dartHostTestHandler: 'TestAudioPlayersApi')
abstract class AudioPlayersApi {
  @async
  void create(String playerId);

  @async
  void dispose(String playerId);

  @async
  int? getCurrentPosition(String playerId);

  @async
  int? getDuration(String playerId);

  @async
  void pause(String playerId);

  @async
  void release(String playerId);

  @async
  void resume(String playerId);

  @async
  void seek(String playerId, int position);

//  @async
//  void setAudioContext(String playerId, AudioContext audioContext);

  @async
  void setBalance(String playerId, double balance);

  @async
  void setPlayerMode(String playerId, String playerMode);

  @async
  void setPlaybackRate(String playerId, double playbackRate);

  @async
  void setReleaseMode(String playerId, String releaseMode);

  @async
  void setSourceBytes(String playerId, Uint8List bytes);

  @async
  void setSourceUrl(String playerId, String url, bool isLocal);

  @async
  void setVolume(String playerId, double volume);

  @async
  void stop(String playerId);

  @async
  void emitLog(String playerId, String message);

  @async
  void emitError(String playerId, String code, String message);
}
