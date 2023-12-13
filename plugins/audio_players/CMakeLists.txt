
pkg_check_modules(GST IMPORTED_TARGET REQUIRED gstreamer-1.0>=1.4)

add_library(plugin_audioplayers STATIC
        audio_players_registry.cc
        audio_players.cc
        audio_player.cc
)
target_link_directories(plugin_audioplayers PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(plugin_audioplayers PUBLIC platform_homescreen flutter_common PkgConfig::GST)