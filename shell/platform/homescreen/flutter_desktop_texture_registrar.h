#pragma once

#include <GLES2/gl2.h>

struct FlutterDesktopEngineState;

struct GL_TEXTURE_2D_DESC {
  /// Target texture of the active texture unit (example GL_TEXTURE_2D or
  /// GL_TEXTURE_RECTANGLE).
  uint32_t target;
  /// The name of the texture.
  uint32_t name;
  /// The texture format (example GL_RGBA8).
  uint32_t format;

  uint32_t width;
  uint32_t height;
  size_t visible_width;
  size_t visible_height;
  void (*release_callback)(void* release_context);
  void* release_context;
};

// State associated with the texture registrar.
struct FlutterDesktopTextureRegistrar {
  // The engine that backs this registrar.
  FlutterDesktopEngineState* engine;

  std::mutex texture_mutex;

  std::unordered_map<int64_t, std::unique_ptr<GL_TEXTURE_2D_DESC>>
      texture_registry;
};
