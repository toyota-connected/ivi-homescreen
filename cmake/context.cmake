#
# Branch
#
execute_process (
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (GIT_BRANCH)
    message(STATUS "GIT Branch ............. ${GIT_BRANCH}")
    add_definitions("-DGIT_BRANCH=\"${GIT_BRANCH}\"")
else ()
    add_definitions ("-DGIT_BRANCH=\"unkown\"")
endif()

#
# Commit Hash
#
execute_process (
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (GIT_COMMIT_HASH)
    message(STATUS "GIT Hash ............... ${GIT_COMMIT_HASH}")
    add_definitions("-DGIT_HASH=\"${GIT_COMMIT_HASH}\"")
else ()
    add_definitions ("-DGIT_HASH=\"unknown\"")
endif()

#
# Clang Variables
#
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    execute_process(
            COMMAND llvm-config --version
            OUTPUT_VARIABLE LLVM_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (LLVM_VERSION)
        message(STATUS "LLVM Version ........... ${LLVM_VERSION}")
    endif()

    execute_process(
            COMMAND llvm-config --prefix
            OUTPUT_VARIABLE LLVM_ROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (LLVM_ROOT)
        message(STATUS "LLVM Root .............. ${LLVM_ROOT}")
        message(STATUS "C++ header path ........ ${LLVM_ROOT}/include/c++/v1/")

        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-isystem${LLVM_ROOT}/include/c++/v1/>)
    endif()

    execute_process(
            COMMAND llvm-config --cmakedir
            OUTPUT_VARIABLE LLVM_CMAKE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (LLVM_CMAKE_DIR)
        message(STATUS "Clang CMake Path ....... ${LLVM_CMAKE_DIR}")
        list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
    endif()

endif()

#
# Toolchain IPO/LTO support
#
cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(
        RESULT IPO_SUPPORT_RESULT
        OUTPUT IPO_SUPPORT_OUTPUT
        LANGUAGES C CXX
)
if (IPO_SUPPORT_RESULT)
    message(STATUS "IPO .................... supported")
else ()
    message(STATUS "IPO .................... not supported: ${IPO_SUPPORT_OUTPUT}")
endif ()
