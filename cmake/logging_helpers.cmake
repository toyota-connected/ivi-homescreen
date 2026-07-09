# cmake/logging_helpers.cmake
#
# Provides ihs_target_add_logging(<target>): links a target against ihs_shared
# and attaches the shell logging mux (shell/logging/logger.hpp). ihs_shared is
# linked unconditionally — it provides the tracing and (later) config surfaces
# regardless of ENABLE_DLT; ENABLE_DLT only adds the DLT logging bridge inside
# it and switches logger.hpp from the stub macros to the live path. Intended
# for the shell binary and for built-in plugin targets.

function(ihs_target_add_logging TARGET)
    if(NOT TARGET ${TARGET})
        message(WARNING
            "ihs_target_add_logging: target '${TARGET}' does not exist")
        return()
    endif()
    if(NOT TARGET ihs_shared)
        message(FATAL_ERROR
            "ihs_target_add_logging: the ihs_shared target does not exist. "
            "Ensure add_subdirectory(shared) runs before this helper is called.")
    endif()

    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/shell/logging
    )
    # Linking ihs_shared also propagates its public include dir, so the ihs/*
    # headers (logging, trace, format) are on the include path.
    target_link_libraries(${TARGET} PRIVATE ivi_homescreen::ihs_shared)

    if(ENABLE_DLT)
        target_compile_definitions(${TARGET} PRIVATE ENABLE_DLT=1)
        ihs_apply_cxx_compat(${TARGET})
    endif()
endfunction()
