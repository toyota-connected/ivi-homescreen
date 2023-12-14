
#include "include/audio_players/audio_players_plugin_c_api.h"

#include <flutter/plugin_registrar_homescreen.h>

#include "audio_players_plugin.h"

void AudioPlayersPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  plugin_audio_players::AudioPlayersPlugin::RegisterWithRegistrar(registrar);
}
