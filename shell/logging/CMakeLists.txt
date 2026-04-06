# shell/logging/CMakeLists.txt
#
# Builds the DLT bridge shared library (libihs_logging_dlt.so). The legacy
# dlt.cc / libdlt.cc sources remain compiled into the homescreen binary via
# shell/CMakeLists.txt; this library is purely additive; it precedes wiring
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
target_link_libraries(ihs_logging PRIVATE ${CMAKE_DL_LIBS})

target_compile_options(ihs_logging PRIVATE
    -Wall -Wextra -fvisibility=hidden
)

set_target_properties(ihs_logging PROPERTIES
    OUTPUT_NAME               ihs_logging_dlt
    CXX_VISIBILITY_PRESET     hidden
    VISIBILITY_INLINES_HIDDEN ON
)
