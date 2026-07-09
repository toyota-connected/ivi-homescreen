# cmake/logging_helpers.cmake
#
# Provides ihs_target_add_logging(<target>): attaches the IHS logging mux
# (shell/logging/logger.hpp, backed by the ihs_shared DLT bridge when
# ENABLE_DLT is on) to an arbitrary target. Intended for the shell binary
# itself and for built-in plugin targets that want to emit IHS_LOG_* calls.
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
        if(NOT TARGET ihs_shared)
            message(FATAL_ERROR
                "ihs_target_add_logging: ENABLE_DLT is ON but the ihs_shared "
                "target does not exist. Ensure add_subdirectory(shared) runs "
                "before this helper is called.")
        endif()
        # Linking ihs_shared also propagates its public include dir, so
        # logger.hpp finds ihs/logging.h and ihs/format.h.
        target_link_libraries(${TARGET} PRIVATE ivi_homescreen::ihs_shared)
        target_compile_definitions(${TARGET} PRIVATE ENABLE_DLT=1)
        ihs_apply_cxx_compat(${TARGET})
    endif()
endfunction()
