
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
add_subdirectory(cxxopts)

#
# tomlplusplus
#
add_subdirectory(tomlplusplus)

#
# Vulkan-Headers
#
add_subdirectory(Vulkan-Headers)

#
# Flutter Common library
#
add_subdirectory(flutter)

#
# waypp
#
if (BUILD_BACKEND_WAYLAND_EGL)
    set(ENABLE_EGL ON)
else ()
    set(ENABLE_EGL OFF)
endif ()
set(LOGGING_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/shell;${CMAKE_BINARY_DIR})
add_subdirectory(waypp)

#
# libliftoff
#
if (BUILD_BACKEND_DRM OR BUILD_BACKEND_WAYLAND_LEASED_DRM)
    add_subdirectory(libliftoff-0.6.0-dev)
endif ()

#
# Google Test
#
if (BUILD_UNIT_TESTS)
    add_subdirectory(googletest)
endif ()
