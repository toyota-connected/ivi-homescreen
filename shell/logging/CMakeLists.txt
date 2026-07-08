# shell/logging/CMakeLists.txt
#
# Builds the DLT bridge as a STATIC library linked into the homescreen binary.
# Static because it has no out-of-tree consumers yet: shipping it as a .so would
# require install() + an $ORIGIN rpath that neither exists. When the ihs_shared
# module lands, these sources relocate into shared/ and ship as part of
# libihs_shared.so (with a proper C ABI, install/export, and SOVERSION); this
# standalone target goes away then. The legacy dlt.cc / libdlt.cc sources are
# compiled straight into homescreen via shell/CMakeLists.txt.

if(NOT ENABLE_DLT)
    return()
endif()

add_library(ihs_logging STATIC
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

# Match the shell's C++ standard library. The `toolchain` INTERFACE library
# applies -stdlib=libc++ on clang builds; without it the bridge compiles against
# libstdc++ and its std::string_view / std::function symbols mismatch the libc++
# homescreen binary at link time (undefined DltBridge::log / acquire_context).
if (TARGET toolchain)
    target_link_libraries(ihs_logging PRIVATE toolchain::toolchain)
endif ()

target_compile_options(ihs_logging PRIVATE -Wall -Wextra)

# The bridge's C++ entry points (DltBridge, ContextCache) and the extern "C"
# ffi_shim surface link straight into the homescreen binary from this archive.
set_target_properties(ihs_logging PROPERTIES
    OUTPUT_NAME ihs_logging_dlt
)
