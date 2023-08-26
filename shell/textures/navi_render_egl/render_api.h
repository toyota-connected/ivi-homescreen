
#pragma once

// #############################################################################
//  General
// #############################################################################

typedef uint32_t NAV_RENDER_API_VERSION_T();

typedef struct NavRender_Context NAV_RENDER_API_CONTEXT_T;

typedef const void* (*GlLoaderFunction)(void* userdata, const char* procname);

typedef void (*LoggerFunction)(int level,
                               const char* context,
                               char const* message);

typedef NAV_RENDER_API_CONTEXT_T* NAV_RENDER_API_INITIALIZE_T(
    const char* accessToken,
    int width,
    int height,
    const char* assetsPath,
    const char* cachePath,
    const char* miscPath);

typedef struct {
  void* dpy;
  void* context;
  uint32_t framebufferId;
  const char* access_token;
  int width;
  int height;
  const char* asset_path;
  const char* cache_folder;
  const char* misc_folder;
  LoggerFunction pfnLogger;
  GlLoaderFunction pfnGlLoader;
  const void* native_window;
} NavRenderConfig;

typedef NAV_RENDER_API_CONTEXT_T* NAV_RENDER_API_INITIALIZE2_T(
    const void* NavRenderConfig);

typedef void NAV_RENDER_API_DE_INITIALIZE_T(NAV_RENDER_API_CONTEXT_T* ctx);

// #############################################################################
//  Map control
// #############################################################################

//
// Main
//

typedef void NAV_RENDER_API_RUN_TASK_T(NAV_RENDER_API_CONTEXT_T* ctx);

typedef void NAV_RENDER_API_RENDER_T(NAV_RENDER_API_CONTEXT_T* ctx,
                                     uint32_t framebufferId);

typedef void NAV_RENDER_API_RENDER2_T(NAV_RENDER_API_CONTEXT_T* ctx);

typedef void NAV_RENDER_API_RESIZE_T(NAV_RENDER_API_CONTEXT_T* ctx,
                                     int width,
                                     int height);
