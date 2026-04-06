# cmake/cxx_compat.cmake
# Detects C++ feature availability and sets IHS_HAS_CXX* variables.
# Include this before any DLT targets are created.

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# ── Helper macro ──────────────────────────────────────────────────────────────
function(ihs_check_cxx_feature VAR SOURCE)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS}")
    if(CMAKE_CXX_STANDARD)
        # check_cxx_source_compiles does not honor CMAKE_CXX_STANDARD; pass
        # the -std= flag explicitly so the probe reflects the real build.
        set(CMAKE_REQUIRED_FLAGS
            "${CMAKE_REQUIRED_FLAGS} -std=c++${CMAKE_CXX_STANDARD}")
    endif()
    # std::jthread (and a handful of other facilities) need pthread linkage
    # under libstdc++; without it the probe link step fails even though the
    # header parses cleanly.
    set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES};-pthread")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -pthread")
    check_cxx_source_compiles("${SOURCE}" ${VAR})
    cmake_pop_check_state()
    set(${VAR} "${${VAR}}" PARENT_SCOPE)
endfunction()

# ── C++20 features ────────────────────────────────────────────────────────────

ihs_check_cxx_feature(IHS_HAS_STD_FORMAT [=[
#include <format>
int main() { return std::format("{}", 42).size() > 0 ? 0 : 1; }
]=])

ihs_check_cxx_feature(IHS_HAS_STD_JTHREAD [=[
#include <thread>
// Note: pass a free function pointer rather than a lambda. libstdc++14
// + Clang-17 + -std=c++23 has a non-SFINAE-friendly tuple check inside
// std::thread's ctor that fires on closure types and would mis-fail this
// probe. A function pointer sidesteps it.
static void ihs_probe_thread_body() {}
int main() { std::jthread t(ihs_probe_thread_body); return 0; }
]=])

ihs_check_cxx_feature(IHS_HAS_FORMAT_TO_N [=[
#include <format>
int main() {
    char buf[32];
    auto r = std::format_to_n(buf, 31, "{}", 42);
    *r.out = 0;
    return 0;
}
]=])

# ── C++23 features ────────────────────────────────────────────────────────────

ihs_check_cxx_feature(IHS_HAS_FLAT_MAP [=[
#include <flat_map>
int main() { std::flat_map<int,int> m; m[1]=2; return 0; }
]=])

ihs_check_cxx_feature(IHS_HAS_STD_EXPECTED [=[
#include <expected>
int main() {
    std::expected<int,int> e{42};
    return e.has_value() ? 0 : 1;
}
]=])

ihs_check_cxx_feature(IHS_HAS_STD_PRINT [=[
#include <print>
int main() { std::println(stderr, "test"); return 0; }
]=])

# ── Summary ───────────────────────────────────────────────────────────────────

message(STATUS "[DLT compat] std::format     : ${IHS_HAS_STD_FORMAT}")
message(STATUS "[DLT compat] std::format_to_n: ${IHS_HAS_FORMAT_TO_N}")
message(STATUS "[DLT compat] std::jthread    : ${IHS_HAS_STD_JTHREAD}")
message(STATUS "[DLT compat] std::flat_map   : ${IHS_HAS_FLAT_MAP}")
message(STATUS "[DLT compat] std::expected   : ${IHS_HAS_STD_EXPECTED}")
message(STATUS "[DLT compat] std::print      : ${IHS_HAS_STD_PRINT}")

# ── Propagate as compile definitions to all DLT bridge sources ────────────────

function(ihs_apply_cxx_compat TARGET)
    foreach(FLAG
            IHS_HAS_STD_FORMAT
            IHS_HAS_FORMAT_TO_N
            IHS_HAS_STD_JTHREAD
            IHS_HAS_FLAT_MAP
            IHS_HAS_STD_EXPECTED
            IHS_HAS_STD_PRINT)
        if(${FLAG})
            target_compile_definitions(${TARGET} PRIVATE ${FLAG}=1)
        endif()
    endforeach()
endfunction()
