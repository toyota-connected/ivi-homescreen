/*
 * Copyright 2020-2024 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>

namespace nav_render_view_plugin {

typedef void (*LoggerFunction)(int level,
                               const char* context,
                               const char* message);
typedef const void* (*GlLoaderFunction)(void* userdata, const char* procname);

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
  LoggerFunction pfn_log;
  GlLoaderFunction pfn_gl_loader;
  const void* native_window;
#if HEADLESS_BACKEND_EGL
  void* contextHeadless;
  unsigned char* buffer;
#endif
} NavRenderConfig;

struct nav_render_Context;

struct LibNavRenderExports {
  LibNavRenderExports() = default;
  explicit LibNavRenderExports(void* lib);

  uint32_t (*TextureGetInterfaceVersion)() = nullptr;
  nav_render_Context* (*TextureInitialize)(const char* accessToken,
                                    int width,
                                    int height,
                                    const char* assetsPath,
                                    const char* cachePath,
                                    const char* miscPath) = nullptr;
  nav_render_Context* (*TextureInitialize2)(NavRenderConfig* config) = nullptr;
  void (*TextureDeInitialize)(nav_render_Context* ctx) = nullptr;
  void (*TextureRunTask)(nav_render_Context* ctx) = nullptr;
  void (*TextureRender)(nav_render_Context* ctx, uint32_t framebufferId) = nullptr;
  void (*TextureRender2)(nav_render_Context* ctx) = nullptr;
  void (*TextureResize)(nav_render_Context* ctx, int width, int height) = nullptr;


  uint32_t (*SurfaceGetInterfaceVersion)() = nullptr;
  nav_render_Context* (*SurfaceInitialize)(const char* accessToken,
                                           int width,
                                           int height,
                                           const void* nativeWindow,
                                           const char* assetsPath,
                                           const char* cachePath,
                                           const char* miscPath) = nullptr;
  void (*SurfaceDeInitialize)(nav_render_Context* ctx) = nullptr;
  void (*SurfaceRunTask)(nav_render_Context* ctx) = nullptr;
  void (*SurfaceDrawFrame)(nav_render_Context* ctx) = nullptr;
  void (*SurfaceResize)(nav_render_Context* ctx, int width, int height) = nullptr;
};

class LibNavRender {
 public:
  static constexpr uint32_t kExpectedSurfaceApiVersion = 0x00010000;
  static constexpr uint32_t kExpectedTextureApiVersion = 0x00010002;

  static bool IsPresent() { return loadExports() != nullptr; }

  LibNavRenderExports* operator->();

 private:
  static LibNavRenderExports* loadExports();
};

extern LibNavRender LibNavRender;

}  // namespace nav_render_view_plugin
