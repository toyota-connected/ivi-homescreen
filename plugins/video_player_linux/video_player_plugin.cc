// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "video_player_plugin.h"

#include <flutter/plugin_registrar_homescreen.h>

#include <map>
#include <memory>
#include <string>

#include "messages.g.h"
#include "video_player.h"

namespace video_player_linux {

// static
void VideoPlayerPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<VideoPlayerPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

VideoPlayerPlugin::VideoPlayerPlugin() = default;
VideoPlayerPlugin::~VideoPlayerPlugin() = default;

VideoPlayerPlugin::VideoPlayerPlugin(flutter::PluginRegistrar* registrar)
    : registrar_(registrar) {
  _textureRegistry = registrar->texture_registrar();
  VideoPlayerApi::SetUp(registrar->messenger(), this);
}

std::optional<FlutterError> VideoPlayerPlugin::Initialize() {
  for (int i = 0; i < videoPlayers.size(); i++) {
    videoPlayers.at((int64_t)i)->Dispose();
  }
  videoPlayers.clear();

  return {};
}

ErrorOr<int64_t> VideoPlayerPlugin::Create(
    const std::string* asset,
    const std::string* uri,
    const std::string* /* package_name */,
    const std::string* /* format_hint */,
    const flutter::EncodableMap& http_headers) {
  std::unique_ptr<VideoPlayer> player;
  if (asset && !asset->empty() && uri && !uri->empty()) {
    try {
      player = std::make_unique<VideoPlayer>(nullptr, uri, http_headers);
    } catch (std::exception& e) {
      return FlutterError("uri_load_failed", e.what());
    }
  } else {
    return FlutterError("not_implemented", "Set either an asset or a uri");
  }

  auto textureId = _textureRegistry->RegisterTexture(&player->texture);

  player->Init(registrar_, textureId);

  videoPlayers.insert(
      std::make_pair(player->GetTextureId(), std::move(player)));

  return textureId;
}

std::optional<FlutterError> VideoPlayerPlugin::Dispose(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->Dispose();
    videoPlayers.erase(texture_id);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetLooping(int64_t texture_id,
                                                          bool is_looping) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SetLooping(is_looping);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetVolume(int64_t texture_id,
                                                         double volume) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SetVolume(volume);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetPlaybackSpeed(
    int64_t texture_id,
    double speed) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SetPlaybackSpeed(speed);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::Play(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->Play();
  }

  return {};
}

ErrorOr<int64_t> VideoPlayerPlugin::Position(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  int64_t position = 0;
  if (searchPlayer != videoPlayers.end()) {
    auto& player = searchPlayer->second;
    if (player->IsValid()) {
      position = player->GetPosition();
      player->SendBufferingUpdate();
    }
  }
  return position;
}

std::optional<FlutterError> VideoPlayerPlugin::SeekTo(int64_t texture_id,
                                                      int64_t position) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SeekTo(position);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::Pause(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->Pause();
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetMixWithOthers(
    bool mix_with_others) {
  mixWithOthers = mix_with_others;

  return {};
}

void VideoPlayerPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef /* registrar */) {
  //  VideoPlayerPlugin::RegisterWithRegistrar(
  //      flutter::PluginRegistrarManager::GetInstance()
  //          ->GetRegistrar<flutter::PluginRegistrar>(registrar));
}
}  // namespace video_player_linux