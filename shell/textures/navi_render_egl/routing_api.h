
#pragma once

// #############################################################################
//  General
// #############################################################################

typedef uint32_t NAV_ROUTING_API_VERSION_T();

typedef struct NavRouting_Context NAV_ROUTING_API_CONTEXT_T;

typedef NAV_ROUTING_API_CONTEXT_T* NAV_ROUTING_API_INITIALIZE_T(
    const char* accessToken,
    const char* assetsPath);

typedef void NAV_ROUTING_API_DE_INITIALIZE_T(NAV_ROUTING_API_CONTEXT_T* ctx);
