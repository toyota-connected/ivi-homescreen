/*
 * Copyright 2022 Toyota Connected North America
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

#include "../include/comp_surf_egl/comp_surf_egl.h"
#include "context.h"

#include <cassert>
#include <iostream>
#include <memory>

// #############################################################################

#define API_EXPORT __attribute__((visibility("default")))

// #############################################################################
//  flutter-auto API
// #############################################################################

extern "C" struct comp_surf_Context {
  std::unique_ptr<CompSurfContext> context;
};

namespace {
inline void checkContext(comp_surf_Context* ctx) {
  if (!ctx) {
    std::cerr << "Context not initialized" << std::endl;
    std::abort();
  }

  assert(ctx->context != nullptr);
}

inline CompSurfContext& getContext(comp_surf_Context* ctx) {
  checkContext(ctx);
  return *ctx->context;
}
}  // namespace

API_EXPORT
uint32_t comp_surf_version() {
  return CompSurfContext::version();
}

API_EXPORT
void comp_surf_load_functions(void* userdata,
                              comp_surf_LoaderFunction loaderFunction) {
  (void)userdata;
  (void)loaderFunction;
}

API_EXPORT
comp_surf_Context* comp_surf_initialize(const char* accessToken,
                                        int width,
                                        int height,
                                        void* nativeWindow,
                                        const char* assetsPath,
                                        const char* cachePath,
                                        const char* miscPath) {
  auto* ctx = new comp_surf_Context;
  ctx->context = std::make_unique<CompSurfContext>(
      accessToken, width, height, nativeWindow, assetsPath, cachePath, miscPath);
  return ctx;
}

API_EXPORT
void comp_surf_de_initialize(comp_surf_Context* ctx) {
  getContext(ctx).de_initialize();
  ctx->context.release();
  delete ctx;
}

API_EXPORT
void comp_surf_run_task(comp_surf_Context* ctx) {
  getContext(ctx).run_task();
}

API_EXPORT
void comp_surf_draw_frame(comp_surf_Context* ctx, uint32_t time) {
  getContext(ctx).draw_frame(ctx->context.get(), time);
}

API_EXPORT
void comp_surf_resize(comp_surf_Context* ctx, int width, int height) {
  getContext(ctx).resize(width, height);
}
