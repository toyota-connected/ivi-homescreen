
#
# ASIO
#
add_library(asio INTERFACE)
target_compile_options(asio INTERFACE -isystem${CMAKE_CURRENT_SOURCE_DIR}/asio/asio/include)
target_compile_definitions(asio INTERFACE
        ASIO_STANDALONE
        ASIO_HEADER_ONLY
        ASIO_NO_TYPEID
        ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
        ASIO_NO_DEPRECATED
)

#
# Speedlog
#
set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_NO_THREAD_ID ON)
set(SPDLOG_BUILD_PIC ON)
set(SPDLOG_SANITIZE_ADDRESS ${SANITIZE_ADDRESS})
add_library(spdlog INTERFACE)
target_compile_options(spdlog INTERFACE -isystem${CMAKE_CURRENT_SOURCE_DIR}/spdlog/include)

#
# RapidJson
#
add_library(rapidjson INTERFACE)
target_compile_definitions(rapidjson INTERFACE -DRAPIDJSON_HAS_STDSTRING)
target_include_directories(rapidjson INTERFACE rapidjson/include)

#
# cxxopts
#
set(CXXOPTS_CXX_STANDARD ${CMAKE_CXX_STANDARD})
add_subdirectory(cxxopts)

#
# tomlplusplus
#
add_library(tomlplusplus_tomlplusplus INTERFACE)
add_library(tomlplusplus::tomlplusplus ALIAS tomlplusplus_tomlplusplus)
target_compile_options(tomlplusplus_tomlplusplus INTERFACE
        -isystem${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus/include
        -Wno-deprecated-literal-operator
        -Wno-conversion
)

#
# Vulkan-Headers
#
# Expose the top-level include dir (not include/vulkan) so the canonical
# <vulkan/vulkan.h> / <vulkan/vulkan.hpp> includes every consumer uses resolve
# from the vendored tree. Pointing at include/vulkan only satisfied bare
# <vulkan.h> (which nothing uses) and let <vulkan/...> silently fall back to the
# system Vulkan SDK on desktop hosts — a source of API/ABI skew, and a hard
# failure when cross-compiling against a sysroot with no Vulkan headers.
add_library(vulkan_headers INTERFACE)
target_include_directories(vulkan_headers INTERFACE Vulkan-Headers/include)

#
# Flutter Common library
#
add_subdirectory(flutter)

#
# waypp — the Wayland C++ wrapper. Only the Wayland backends (and headless,
# which links wayland-gen) consume it; config/common.h includes <waypp/waypp.h>
# under the same condition. Building it for the DRM EGL/Vulkan and software
# backends would drag Wayland headers + libwayland into builds that must stay
# Wayland-free, so gate the whole subproject.
#
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN)
    if (BUILD_BACKEND_WAYLAND_EGL)
        set(ENABLE_EGL ON)
    else ()
        set(ENABLE_EGL OFF)
    endif ()
    set(LOGGING_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/shell;${CMAKE_BINARY_DIR})
    add_subdirectory(waypp)

    # waypp is a git submodule we keep pristine, so apply the "system library"
    # treatment from here rather than editing it: mark its interface include
    # dirs SYSTEM so its headers are -isystem for consumers, and silence GCC's
    # benign -Wpsabi std::pair ABI note (seat/pointer.h get_xy()) that -isystem
    # does not suppress, emitted while compiling waypp's own sources.
    foreach (_tgt waypp wayland-gen)
        if (TARGET ${_tgt})
            get_target_property(_waypp_inc ${_tgt} INTERFACE_INCLUDE_DIRECTORIES)
            if (_waypp_inc)
                set_target_properties(${_tgt} PROPERTIES
                        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_waypp_inc}")
            endif ()
        endif ()
    endforeach ()
    if (TARGET waypp)
        # Silence waypp's own-source diagnostics (the -Wpsabi std::pair note,
        # and -Wunused-parameter on EGL stubs when ENABLE_EGL is off) — pristine
        # vendor.
        target_compile_options(waypp PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
    endif ()
endif ()

#
# Google Test
#
if (BUILD_UNIT_TESTS)
    add_subdirectory(googletest)

    target_compile_options(gtest PRIVATE -include cstdint)
    get_target_property(TOOLCHAIN_COMPILE_OPTIONS toolchain INTERFACE_COMPILE_OPTIONS)
    if (TOOLCHAIN_COMPILE_OPTIONS)
        target_compile_options(gtest PRIVATE ${TOOLCHAIN_COMPILE_OPTIONS})
    endif ()
    # GCC 15 promotes a signed-conversion warning inside googletest itself
    # (gtest.cc: kSize template parameter). Demote to a warning for
    # third_party only — the project's own Werror settings stay intact.
    target_compile_options(gtest PRIVATE
            -Wno-error=sign-conversion
            -Wno-error=conversion)
    target_compile_options(gmock PRIVATE -Wno-error=deprecated-copy)

    target_compile_options(gtest_main PRIVATE -include cstdint)
    get_target_property(TOOLCHAIN_COMPILE_OPTIONS toolchain INTERFACE_COMPILE_OPTIONS)
    if (TOOLCHAIN_COMPILE_OPTIONS)
        target_compile_options(gtest_main PRIVATE ${TOOLCHAIN_COMPILE_OPTIONS})
    endif ()
    target_compile_options(gtest_main PRIVATE
            -Wno-error=sign-conversion
            -Wno-error=conversion)

    target_compile_options(gmock PRIVATE -Wno-error=deprecated-copy)

endif ()
