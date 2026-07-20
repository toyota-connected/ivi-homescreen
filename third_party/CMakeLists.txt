
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
# fmt (header-only) — formatting for the logging.h shim and the FML
# diagnostics, replacing spdlog's bundled copy.
#
add_library(fmt INTERFACE)
target_compile_options(fmt INTERFACE -isystem${CMAKE_CURRENT_SOURCE_DIR}/fmt/include)
target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY)

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
# Dear ImGui — the compositor debug HUD (Vulkan and/or OpenGL-ES render
# backend, per the enabled ivi backends; see imgui/CMakeLists.txt).
#
if (BUILD_HUD AND (BUILD_BACKEND_WAYLAND_VULKAN OR BUILD_BACKEND_DRM_KMS_VULKAN OR
        BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_DRM_KMS_EGL))
    add_subdirectory(imgui)
endif ()

#
# Flutter Common library
#
add_subdirectory(flutter)

# The Wayland C++ binding layer (wayland-cxx-scanner) is wired up in
# cmake/wayland_cxx.cmake, included from the top-level CMakeLists.txt so it can
# run after compiler.cmake (toolchain ABI) — the same pattern drm-cxx uses.

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
