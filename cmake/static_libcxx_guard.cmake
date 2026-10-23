# cmake/static_libcxx_guard.cmake
#
# ihs_static_libcxx_guard(<target>): fails the build when a target asked to
# carry its C++ runtime statically still has a DT_NEEDED on one. Post-build, so
# the check sees the real link line.
include_guard()

function(ihs_static_libcxx_guard target)
    if (NOT ENABLE_STATIC_LINK)
        return()
    endif ()
    if (NOT STDLIB STREQUAL "libc++")
        return()
    endif ()
    if (NOT CMAKE_READELF)
        message(WARNING "static libc++ guard skipped for ${target}: no readelf")
        return()
    endif ()
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
            -DREADELF=${CMAKE_READELF}
            -DBINARY=$<TARGET_FILE:${target}>
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/static_libcxx_check.cmake
            VERBATIM
            COMMENT "Checking ${target} carries its own libc++")
endfunction()
