
#
# ASIO
#
add_subdirectory(asio-1-28-1)

#
# Speedlog
#
set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_NO_THREAD_ID ON)
# set(SPDLOG_BUILD_PIC ON)
set(SPDLOG_SANITIZE_ADDRESS ${SANITIZE_ADDRESS})
add_library(spdlog INTERFACE)
target_include_directories(spdlog INTERFACE spdlog-1.12.0/include)
#add_subdirectory(spdlog-1.12.0)

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

target_link_libraries(flutter PUBLIC spdlog)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET flutter PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

add_sanitizers(flutter)

#
# Google Test
#
if (BUILD_UNIT_TESTS)
    add_subdirectory(googletest)
endif ()
