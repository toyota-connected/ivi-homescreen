# shell/logging/tests/load/CMakeLists.txt
#
# Standalone throughput/drop/latency load test for the DLT bridge. Builds the
# ihs_logging library (shell/logging), a dlt_stub shared object standing in for
# libdlt.so.2, and the dlt_load_test driver. Like the compat-matrix harness it
# needs only a C++ toolchain — no Wayland / Flutter / other submodules — so it
# runs on a bare CI image.
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

# libdlt.so.2 stand-in — pointed at via IHS_DLT_LIBRARY at run time.
add_library(dlt_stub SHARED dlt_stub.cc)
set_target_properties(dlt_stub PROPERTIES CXX_VISIBILITY_PRESET default)

if(ENABLE_DLT)
    add_subdirectory(${IHS_ROOT}/shell/logging
                     ${CMAKE_BINARY_DIR}/shell_logging)
endif()

add_executable(dlt_load_test dlt_load_test.cc)
target_include_directories(dlt_load_test PRIVATE ${IHS_ROOT}/shell/logging)
target_link_libraries(dlt_load_test PRIVATE ${CMAKE_DL_LIBS})

if(ENABLE_DLT)
    target_link_libraries(dlt_load_test PRIVATE ihs_logging)
    target_compile_definitions(dlt_load_test PRIVATE ENABLE_DLT=1)
    ihs_apply_cxx_compat(dlt_load_test)
endif()

target_compile_options(dlt_load_test PRIVATE -Wall -Wextra)

# A short drop-free run at a modest rate as a CI smoke gate. Longer/heavier
# sweeps (max-throughput, overload) are run manually or on a perf runner.
enable_testing()
add_test(NAME dlt_load_smoke
         COMMAND dlt_load_test 4 2 50000)
set_tests_properties(dlt_load_smoke PROPERTIES
    ENVIRONMENT "IHS_DLT_LIBRARY=$<TARGET_FILE:dlt_stub>")
