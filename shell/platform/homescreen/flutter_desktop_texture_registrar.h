#pragma once

#include <GLES2/gl2.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <flutter_texture_registrar.h>
#include <shell/platform/embedder/embedder.h>

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

  // Pixel-buffer mode. When |pixel_buffer_callback| is non-null the embedder
  // owns the GL texture named by |name| (lazy-allocated on first upload from
  // the raster thread); for GPU-surface mode the plugin owns the GL texture
  // and this pair stays null.
  FlutterDesktopPixelBufferTextureCallback pixel_buffer_callback;
  void* pixel_buffer_user_data;
};

// State associated with the texture registrar.
struct FlutterDesktopTextureRegistrar {
  // The engine that backs this registrar.
  FlutterDesktopEngineState* engine;

  std::mutex texture_mutex;

  std::unordered_map<int64_t, std::unique_ptr<GL_TEXTURE_2D_DESC>>
      texture_registry;

  // Latched by ~FlutterView before m_state destructs. Plugin streaming
  // threads (e.g. video_player gstreamer handoff) keep firing texture
  // callbacks past the point where m_state (and the engine_state /
  // view_controller this registrar points at) have been freed; without
  // this gate they deref dangling pointers and SEGV. Texture-callback
  // entry points in flutter_desktop.cc check this flag and return early.
  std::atomic<bool> shutting_down{false};
};

// Resolve a registered external texture for the Flutter engine's
// |gl_external_texture_frame_callback|. For pixel-buffer textures this
// invokes the plugin callback, lazily allocates the embedder-owned GL
// texture, and uploads the pixel data. For GPU-surface textures it returns
// the plugin-owned GL texture unchanged. Must be called with the raster GL
// context current. Returns false if |texture_id| is not registered or the
// plugin callback produced no frame.
bool PopulateExternalGlTextureFrame(
    FlutterDesktopTextureRegistrar* texture_registrar,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture* texture_out);
