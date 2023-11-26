
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

add_library(flutter STATIC ${FLUTTER_SRC_FILES})

target_compile_options(flutter PRIVATE ${CONTEXT_COMPILE_OPTIONS})
target_include_directories(flutter PUBLIC
        .
        flutter
        flutter/third_party
        flutter/third_party/rapidjson/include
        flutter/shell/platform/common/public
        flutter/shell/platform/common/client_wrapper/include
        flutter/shell/platform/common/client_wrapper/include/flutter
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

if (BUILD_UNIT_TESTS)
    add_subdirectory(googletest)
endif ()

#
# Firebase C++ SDK
#
if (EXISTS ${FIREBASE_CPP_SDK_ROOT_DIR})
    set(FIREBASE_PLUGIN_INCLUDE_DIRS
            ${FIREBASE_CPP_SDK_ROOT_DIR}/app/src/include
            ${FIREBASE_CPP_SDK_ROOT_DIR}/auth/src/include
            ${FIREBASE_CPP_SDK_ROOT_DIR}/database/src/include
            PARENT_SCOPE)
    set(FIREBASE_PLUGIN_LIB_DIRS
            ${FIREBASE_CPP_SDK_BINARY_DIR}/installations
            ${FIREBASE_CPP_SDK_BINARY_DIR}/messaging
            ${FIREBASE_CPP_SDK_BINARY_DIR}/analytics
            ${FIREBASE_CPP_SDK_BINARY_DIR}/app_check
            ${FIREBASE_CPP_SDK_BINARY_DIR}/database
            ${FIREBASE_CPP_SDK_BINARY_DIR}/dynamic_links
            ${FIREBASE_CPP_SDK_BINARY_DIR}/remote_config
            ${FIREBASE_CPP_SDK_BINARY_DIR}/auth
            ${FIREBASE_CPP_SDK_BINARY_DIR}/app/rest
            ${FIREBASE_CPP_SDK_BINARY_DIR}/app
            ${FIREBASE_CPP_SDK_BINARY_DIR}/storage
            ${FIREBASE_CPP_SDK_BINARY_DIR}/functions
            ${FIREBASE_CPP_SDK_BINARY_DIR}/gma
            ${FIREBASE_CPP_SDK_BINARY_DIR}/external/src/zlib-build
            ${FIREBASE_CPP_SDK_BINARY_DIR}/external/src/flatbuffers-build
            PARENT_SCOPE)

    message(STATUS "Firebase CPP SDK Root: ${FIREBASE_CPP_SDK_ROOT_DIR}")
    message(STATUS "FIREBASE_PLUGIN_INCLUDE_DIRS: ${FIREBASE_PLUGIN_INCLUDE_DIRS}")
    message(STATUS "FIREBASE_PLUGIN_LIB_DIRS: ${FIREBASE_PLUGIN_LIB_DIRS}")
endif ()
