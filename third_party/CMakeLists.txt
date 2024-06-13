
#
# ASIO
#
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE asio/asio/include)
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
target_include_directories(spdlog INTERFACE spdlog/include)

#
# RapidJson
#
add_library(rapidjson INTERFACE)
target_compile_definitions(rapidjson INTERFACE -DRAPIDJSON_HAS_STDSTRING)
target_include_directories(rapidjson INTERFACE rapidjson/include)

#
# Flutter Common library
#

add_library(flutter STATIC
        flutter/shell/platform/common/client_wrapper/core_implementations.cc
        flutter/shell/platform/common/client_wrapper/plugin_registrar.cc
        flutter/shell/platform/common/client_wrapper/standard_codec.cc
        flutter/shell/platform/common/incoming_message_dispatcher.cc
        flutter/shell/platform/common/json_message_codec.cc
        flutter/shell/platform/common/json_method_codec.cc
        flutter/shell/platform/common/path_utils.cc
        flutter/shell/platform/common/text_editing_delta.cc
        flutter/shell/platform/common/text_input_model.cc

        flutter/fml/platform/linux/paths_linux.cc

        flutter/fml/platform/posix/file_posix.cc
        flutter/fml/platform/posix/paths_posix.cc
        flutter/fml/platform/posix/posix_wrappers_posix.cc
        flutter/fml/platform/posix/shared_mutex_posix.cc

        flutter/fml/command_line.cc
        flutter/fml/file.cc
        flutter/fml/log_settings.cc
        flutter/fml/log_settings_state.cc
        flutter/fml/logging.cc
        flutter/fml/paths.cc
        flutter/fml/string_conversion.cc
        flutter/fml/unique_fd.cc
)

target_compile_definitions(flutter PUBLIC RAPIDJSON_HAS_STDSTRING)

target_compile_options(flutter PUBLIC ${CONTEXT_COMPILE_OPTIONS})

target_include_directories(flutter PUBLIC
        .
        flutter/.
        flutter/third_party
        flutter/third_party/rapidjson/include
        flutter/shell/platform/common/public
        flutter/shell/platform/common/client_wrapper/include
        flutter/shell/platform/common/client_wrapper/include/flutter
)

target_link_libraries(flutter PUBLIC
        rapidjson
        spdlog
)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET flutter PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

add_sanitizers(flutter)

#
# waypp
#
if (BUILD_BACKEND_WAYLAND_EGL)
    set(ENABLE_EGL ON)
else ()
    set(ENABLE_EGL OFF)
endif ()
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
