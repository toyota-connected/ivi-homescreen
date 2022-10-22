
#pragma once

#include <cstdint>

// #############################################################################
//  General
// #############################################################################

typedef uint32_t COMP_SURF_API_VERSION_T();

typedef struct CompSurf_Context COMP_SURF_API_CONTEXT_T;

typedef const void* (*LoaderFunction)(void* userdata, char const* procname);

typedef void COMP_SURF_API_LOAD_FUNCTIONS(void*, LoaderFunction);

typedef COMP_SURF_API_CONTEXT_T* COMP_SURF_API_INITIALIZE_T(
    const char* accessToken,
    int width,
    int height,
    void* nativeWindow,
    const char* assetsPath);

typedef void COMP_SURF_API_DE_INITIALIZE_T(COMP_SURF_API_CONTEXT_T* ctx);

// #############################################################################
//  Rendering
// #############################################################################

typedef void COMP_SURF_API_RENDER_T(COMP_SURF_API_CONTEXT_T* ctx,
                                     double time);

typedef void COMP_SURF_API_RESIZE_T(COMP_SURF_API_CONTEXT_T* ctx,
                                     int width,
                                     int height);
