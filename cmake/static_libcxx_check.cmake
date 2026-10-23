# cmake/static_libcxx_check.cmake
#
# Script half of ihs_static_libcxx_guard(); run with -DREADELF= and -DBINARY=.
execute_process(
        COMMAND ${READELF} -d ${BINARY}
        OUTPUT_VARIABLE _dyn
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err)
if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "readelf -d failed on ${BINARY}: ${_err}")
endif ()

# literal substring per line, not regex: every soname here contains '+', which
# CMake's regex engine reads as a repeat operator and refuses to compile
string(REPLACE "\n" ";" _lines "${_dyn}")
set(_found "")
foreach (_line IN LISTS _lines)
    string(FIND "${_line}" "(NEEDED)" _needed)
    if (_needed EQUAL -1)
        continue()
    endif ()
    foreach (_soname libc++.so libc++abi.so libunwind.so libstdc++.so)
        string(FIND "${_line}" "${_soname}" _hit)
        if (NOT _hit EQUAL -1)
            list(APPEND _found ${_soname})
        endif ()
    endforeach ()
endforeach ()

if (_found)
    list(REMOVE_DUPLICATES _found)
    string(REPLACE ";" ", " _list "${_found}")
    message(FATAL_ERROR
            "${BINARY}: ENABLE_STATIC_LINK is on and the C++ runtime is libc++, "
            "but DT_NEEDED still names: ${_list}")
endif ()
