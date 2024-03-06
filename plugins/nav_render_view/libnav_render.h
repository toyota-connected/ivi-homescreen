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
#if defined(HEADLESS_BACKEND)
  void* contextHeadless;
  unsigned char* buffer;
#endif
} NavRenderConfig;

struct nav_render_Context;

struct LibNavRenderExports {
  LibNavRenderExports() = default;
  explicit LibNavRenderExports(void* lib);

  uint32_t (*GetInterfaceVersion)() = nullptr;
  nav_render_Context* (*Initialize)(const char* accessToken,
                                    int width,
                                    int height,
                                    const char* assetsPath,
                                    const char* cachePath,
                                    const char* miscPath) = nullptr;
  nav_render_Context* (*Initialize2)(NavRenderConfig* config) = nullptr;
  void (*DeInitialize)(nav_render_Context* ctx) = nullptr;
  void (*RunTask)(nav_render_Context* ctx) = nullptr;
  void (*Render)(nav_render_Context* ctx, uint32_t framebufferId) = nullptr;
  void (*Render2)(nav_render_Context* ctx) = nullptr;
  void (*Resize)(nav_render_Context* ctx, int width, int height) = nullptr;
};

class LibNavRender {
 public:
  static bool IsPresent() { return loadExports() != nullptr; }

  LibNavRenderExports* operator->();

 private:
  static LibNavRenderExports* loadExports();
};

extern LibNavRender LibNavRender;

}  // namespace nav_render_view_plugin
