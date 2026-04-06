# cmake/logging_helpers.cmake
#
# Provides ihs_target_add_logging(<target>): attaches the IHS logging mux
# (shell/logging/logger.hpp and the DLT bridge when ENABLE_DLT is on) to an
# arbitrary target. Intended for the shell binary itself and for built-in
# plugin targets that want to emit IHS_LOG_* calls.
#
# When ENABLE_DLT is OFF the helper is a no-op on link lines but still adds
# the include path so logger.hpp's stub macros compile unchanged.

function(ihs_target_add_logging TARGET)
    if(NOT TARGET ${TARGET})
        message(WARNING
            "ihs_target_add_logging: target '${TARGET}' does not exist")
        return()
    endif()

    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/shell/logging
    )

    if(ENABLE_DLT)
        if(NOT TARGET ihs_logging)
            message(FATAL_ERROR
                "ihs_target_add_logging: ENABLE_DLT is ON but the "
                "ihs_logging target has not been created yet. Include "
                "add_subdirectory(shell/logging) before calling this helper.")
        endif()
        target_link_libraries(${TARGET} PRIVATE ihs_logging)
        target_compile_definitions(${TARGET} PRIVATE ENABLE_DLT=1)
        ihs_apply_cxx_compat(${TARGET})
    endif()
endfunction()
