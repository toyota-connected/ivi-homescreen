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

#ifndef COMP_SURF_EGL_H
#define COMP_SURF_EGL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// #############################################################################
//  Data
// #############################################################################

typedef struct comp_surf_Context comp_surf_Context;

// #############################################################################
//  flutter-auto API
// #############################################################################

uint32_t comp_surf_version();

typedef const void* (*comp_surf_LoaderFunction)(void* userdata,
                                                char const* symbol_name);
void comp_surf_load_functions(void* userdata,
                              comp_surf_LoaderFunction loaderFunction);

comp_surf_Context* comp_surf_initialize(const char* accessToken,
                                        int width,
                                        int height,
                                        void* nativeWindow,
                                        const char* assetsPath,
                                        const char* cachePath,
                                        const char* miscPath);

void comp_surf_de_initialize(comp_surf_Context* ctx);

void comp_surf_run_task(comp_surf_Context* ctx);

void comp_surf_draw_frame(comp_surf_Context* ctx, uint32_t time);

void comp_surf_resize(comp_surf_Context* ctx, int width, int height);

// #############################################################################
//  services API
// #############################################################################

#ifdef __cplusplus
}
#endif

#endif  // COMP_SURF_EGL_H
