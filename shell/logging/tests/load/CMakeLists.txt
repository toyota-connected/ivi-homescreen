# shell/logging/tests/load/CMakeLists.txt
#
# Standalone throughput/drop/latency load test for the DLT bridge. A whitebox
# test: it compiles the bridge sources (now in shared/src/logging) directly
# into the dlt_load_test driver, alongside a dlt_stub shared object standing in
# for libdlt.so.2. Like the compat-matrix harness it needs only a C++ toolchain
# — no Wayland / Flutter / other submodules — so it runs on a bare CI image.
#
# Run:  IHS_DLT_LIBRARY=$<TARGET_FILE:dlt_stub> ./dlt_load_test [threads] [secs] [rate]

cmake_minimum_required(VERSION 3.16)
project(ihs_logging_load CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

get_filename_component(IHS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.."
                       ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${IHS_ROOT}/cmake")

option(ENABLE_DLT "Enable DLT logging" ON)
include(cxx_compat)

set(IHS_SHARED_DIR ${IHS_ROOT}/shared)

# libdlt.so.2 stand-in — pointed at via IHS_DLT_LIBRARY at run time.
add_library(dlt_stub SHARED dlt_stub.cc)
set_target_properties(dlt_stub PROPERTIES CXX_VISIBILITY_PRESET default)

# No-op export header standing in for the generate_export_header output; the
# bridge sources are compiled straight into the driver here (nothing exported).
set(IHS_GEN_INC ${CMAKE_BINARY_DIR}/ihs_gen)
file(WRITE ${IHS_GEN_INC}/ihs/ihs_export.h
"#ifndef IHS_EXPORT_H\n#define IHS_EXPORT_H\n#define IHS_EXPORT\n#define IHS_NO_EXPORT\n#endif\n")

set(IHS_BRIDGE_SRCS
    ${IHS_SHARED_DIR}/src/logging/libdlt_loader.cpp
    ${IHS_SHARED_DIR}/src/logging/thread_ring.cpp
    ${IHS_SHARED_DIR}/src/logging/ring_registry.cpp
    ${IHS_SHARED_DIR}/src/logging/context_cache.cpp
    ${IHS_SHARED_DIR}/src/logging/worker.cpp
    ${IHS_SHARED_DIR}/src/logging/bridge.cpp
    ${IHS_SHARED_DIR}/src/logging/ffi_shim.cpp
)

add_executable(dlt_load_test dlt_load_test.cc)
target_link_libraries(dlt_load_test PRIVATE ${CMAKE_DL_LIBS})

if(ENABLE_DLT)
    target_sources(dlt_load_test PRIVATE ${IHS_BRIDGE_SRCS})
    target_include_directories(dlt_load_test PRIVATE
        ${IHS_SHARED_DIR}/src/logging
        ${IHS_SHARED_DIR}/include
        ${IHS_GEN_INC}
        ${IHS_ROOT}/shell/logging
    )
    target_compile_definitions(dlt_load_test PRIVATE ENABLE_DLT=1)
    find_package(Threads REQUIRED)
    target_link_libraries(dlt_load_test PRIVATE Threads::Threads)
    ihs_apply_cxx_compat(dlt_load_test)
else()
    target_include_directories(dlt_load_test PRIVATE ${IHS_ROOT}/shell/logging)
endif()

target_compile_options(dlt_load_test PRIVATE -Wall -Wextra)

# A short drop-free run at a modest rate as a CI smoke gate. Longer/heavier
# sweeps (max-throughput, overload) are run manually or on a perf runner.
enable_testing()
add_test(NAME dlt_load_smoke
         COMMAND dlt_load_test 4 2 50000)
set_tests_properties(dlt_load_smoke PROPERTIES
    ENVIRONMENT "IHS_DLT_LIBRARY=$<TARGET_FILE:dlt_stub>")
