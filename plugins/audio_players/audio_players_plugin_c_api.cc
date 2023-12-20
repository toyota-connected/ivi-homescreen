
#include "include/audio_players/audio_players_plugin_c_api.h"

#include <flutter/plugin_registrar.h>

#include "audio_players_plugin.h"

void AudioPlayersPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  plugin_audio_players::AudioPlayersPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrar>(registrar));
}
