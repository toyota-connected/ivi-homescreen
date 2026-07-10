# shell/logging/tests/bench/CMakeLists.txt
#
# Producer-latency regression guard for the shell logging path. Unlike the
# whitebox load test, this links the INSTALLED libihs_shared so it measures the
# real cross-.so FFI cost the shell pays (ihs_log_enabled + ihs_log through the
# PLT), plus the logging.h fmt formatting. The spdlog reference (bundled,
# header-only) is formatted into a discarding sink on the same machine so the
# pass/fail ratio is runner-independent.
#
# Build against an installed ihs_shared (see .github/workflows/ihs-shared.yml):
#   cmake -S shell/logging/tests/bench -B b -DCMAKE_PREFIX_PATH=<prefix> \
#         -DIHS_ROOT=<repo>
#   cmake --build b && ctest --test-dir b --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(ihs_logging_bench CXX)

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Repo root, for the shell logging headers and spdlog's bundled fmt.
if (NOT DEFINED IHS_ROOT)
    get_filename_component(IHS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.."
            ABSOLUTE)
endif ()

find_package(ivi-homescreen-shared CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(logging_bench logging_bench.cc)
target_compile_definitions(logging_bench PRIVATE FMT_HEADER_ONLY)
# Shell files include "logging/logging.h" relative to shell/.
target_include_directories(logging_bench PRIVATE ${IHS_ROOT}/shell)
# spdlog's bundled fmt + sinks (headers only) for the same-machine reference.
target_include_directories(logging_bench SYSTEM PRIVATE
        ${IHS_ROOT}/third_party/spdlog/include)
target_link_libraries(logging_bench PRIVATE
        ivi_homescreen::ihs_shared Threads::Threads)
target_compile_options(logging_bench PRIVATE -O2 -Wall -Wextra)

enable_testing()
# Two modes, each self-asserting a runner-independent ratio + absolute ceiling.
add_test(NAME logging_bench_enabled COMMAND logging_bench enabled 500000)
add_test(NAME logging_bench_filtered COMMAND logging_bench filtered 500000)
