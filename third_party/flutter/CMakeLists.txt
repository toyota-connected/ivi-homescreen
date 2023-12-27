#
# Flutter Engine
#

add_library(flutter STATIC
        shell/platform/common/client_wrapper/core_implementations.cc
        shell/platform/common/client_wrapper/plugin_registrar.cc
        shell/platform/common/client_wrapper/standard_codec.cc
        shell/platform/common/incoming_message_dispatcher.cc
        shell/platform/common/json_message_codec.cc
        shell/platform/common/json_method_codec.cc
        shell/platform/common/path_utils.cc
        shell/platform/common/text_editing_delta.cc
        shell/platform/common/text_input_model.cc

        fml/platform/linux/paths_linux.cc

        fml/platform/posix/file_posix.cc
        fml/platform/posix/paths_posix.cc
        fml/platform/posix/posix_wrappers_posix.cc
        fml/platform/posix/shared_mutex_posix.cc

        fml/command_line.cc
        fml/file.cc
        fml/log_settings.cc
        fml/log_settings_state.cc
        fml/logging.cc
        fml/paths.cc
        fml/string_conversion.cc
        fml/unique_fd.cc
)

target_compile_definitions(flutter PUBLIC RAPIDJSON_HAS_STDSTRING)

target_compile_options(flutter PRIVATE ${CONTEXT_COMPILE_OPTIONS})

target_include_directories(flutter PUBLIC
        .
        ..
        third_party
        third_party/rapidjson/include
        shell/platform/common/public
        shell/platform/common/client_wrapper/include
        shell/platform/common/client_wrapper/include/flutter
)

target_link_libraries(flutter PUBLIC spdlog)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET flutter PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

add_sanitizers(flutter)
