
set(LINUX TRUE)

add_compile_definitions(FILAMENT_SUPPORTS_WAYLAND)

include_directories(.)

if (BUILD_BACKEND_WAYLAND_VULKAN)
    add_subdirectory(filament/libs/bluevk)
endif ()

#
# Flutter Engine
#
set(FLUTTER_SRC_FILES
        flutter/shell/platform/common/client_wrapper/core_implementations.cc
        flutter/shell/platform/common/client_wrapper/plugin_registrar.cc
        flutter/shell/platform/common/client_wrapper/standard_codec.cc
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

add_library(flutter STATIC ${FLUTTER_SRC_FILES})

target_compile_options(flutter PRIVATE ${CONTEXT_COMPILE_OPTIONS})
target_include_directories(flutter PUBLIC
        .
        flutter
        flutter/third_party
        flutter/third_party/rapidjson/include
        flutter/shell/platform/common/public
        flutter/shell/platform/common/client_wrapper/include
        spdlog-1.12.0/include
        )

if (IPO_SUPPORT_RESULT)
    set_property(TARGET flutter PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

add_sanitizers(flutter)

if (BUILD_CRASH_HANDLER)
    find_package(sentry REQUIRED GLOBAL)
endif ()

set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_NO_THREAD_ID ON)
set(SPDLOG_BUILD_PIC ON)
add_subdirectory(spdlog-1.12.0)