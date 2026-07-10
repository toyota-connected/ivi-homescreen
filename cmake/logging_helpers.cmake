# cmake/logging_helpers.cmake
#
# Provides ihs_target_add_logging(<target>): links a target against ihs_shared
# and attaches the shell logging surface (shell/logging/{logger.hpp,logging.h}).
# ihs_shared is linked unconditionally — the logging, tracing, and config
# surfaces are always present; ENABLE_DLT only adds the DLT sink inside it. The
# logging.h shim formats with fmt ("{}" style) before the C ABI, so the target
# also gets fmt's headers via the spdlog interface library (bundled fmt).
# Intended for the shell binary and for built-in plugin targets.

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

    # PUBLIC, not PRIVATE: a library target (e.g. platform_homescreen) exports
    # PUBLIC sources — platform_views/*.cc — that its dependents (the plugins)
    # compile themselves, and those sources include logging.h. The ihs_shared /
    # spdlog include paths must therefore propagate to dependents, or the plugin
    # build fails to find ihs/logging.h. For a leaf executable PUBLIC is
    # equivalent to PRIVATE (nothing links it).
    target_include_directories(${TARGET} PUBLIC
        ${CMAKE_SOURCE_DIR}/shell/logging
    )
    # Linking ihs_shared also propagates its public include dir, so the ihs/*
    # headers (logging, trace, format) are on the include path. spdlog is an
    # interface target that supplies the (bundled) fmt headers the logging.h
    # shim formats with.
    target_link_libraries(${TARGET} PUBLIC ivi_homescreen::ihs_shared)
    if(TARGET spdlog)
        target_link_libraries(${TARGET} PUBLIC spdlog)
    endif()

    if(ENABLE_DLT)
        target_compile_definitions(${TARGET} PRIVATE ENABLE_DLT=1)
        ihs_apply_cxx_compat(${TARGET})
    endif()
endfunction()
