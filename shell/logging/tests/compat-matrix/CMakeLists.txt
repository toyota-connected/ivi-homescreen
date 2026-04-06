# shell/logging/tests/compat-matrix/CMakeLists.txt
#
# Standalone compat-matrix harness for the DLT bridge. Pulls in just the
# ihs_logging library (shell/logging) plus a compat_smoke executable that
# exercises the mux header's macros and the bridge's ring/worker path.
#
# Used by .github/workflows/ihs-logging-compat-matrix.yml. Does not depend
# on Wayland, Flutter engine, or any of the other homescreen third_party
# submodules, so it can run on a bare Ubuntu image with only a C++ toolchain.

cmake_minimum_required(VERSION 3.16)
project(ihs_logging_compat_matrix CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Locate the ivi-homescreen source root (three levels up from this file).
get_filename_component(IHS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.."
                       ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH "${IHS_ROOT}/cmake")

option(ENABLE_DLT "Enable DLT logging" ON)

include(cxx_compat)

if(ENABLE_DLT)
    add_subdirectory(${IHS_ROOT}/shell/logging
                     ${CMAKE_BINARY_DIR}/shell_logging)
endif()

add_executable(compat_smoke compat_smoke.cc)
target_include_directories(compat_smoke PRIVATE
    ${IHS_ROOT}/shell/logging
)

if(ENABLE_DLT)
    target_link_libraries(compat_smoke PRIVATE ihs_logging)
    target_compile_definitions(compat_smoke PRIVATE ENABLE_DLT=1)
    ihs_apply_cxx_compat(compat_smoke)
endif()

target_compile_options(compat_smoke PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME compat_smoke COMMAND compat_smoke)
