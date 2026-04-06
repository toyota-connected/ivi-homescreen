# shell/logging/CMakeLists.txt
#
# Builds the DLT bridge shared library (libihs_logging_dlt.so). The legacy
# dlt.cc / libdlt.cc sources remain compiled into the homescreen binary via
# shell/CMakeLists.txt; this library is purely additive until Phase 3 wires
# main.cc to the mux header.

if(NOT ENABLE_DLT)
    return()
endif()

add_library(ihs_logging SHARED
    dlt/libdlt_loader.cpp
    dlt/thread_ring.cpp
    dlt/ring_registry.cpp
    dlt/context_cache.cpp
    dlt/worker.cpp
    dlt/bridge.cpp
    dlt/ffi_shim.cpp
)

target_include_directories(ihs_logging
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Apply the IHS_HAS_* feature flags detected in cmake/cxx_compat.cmake.
ihs_apply_cxx_compat(ihs_logging)

target_compile_definitions(ihs_logging PUBLIC ENABLE_DLT=1)

find_package(Threads REQUIRED)
target_link_libraries(ihs_logging PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

target_compile_options(ihs_logging PRIVATE -Wall -Wextra)

# Note: the bridge's C++ entry points (DltBridge, ContextCache, etc.) are
# consumed directly by the homescreen binary, so we keep default symbol
# visibility. The extern "C" ffi_shim surface is already public.
set_target_properties(ihs_logging PROPERTIES
    OUTPUT_NAME ihs_logging_dlt
)
