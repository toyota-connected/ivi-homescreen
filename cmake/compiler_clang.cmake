include_guard()

cmake_policy(SET CMP0075 NEW)

if (NOT LLVM_CONFIG)
    string(REGEX MATCH "^[0-9]+" _clang_major "${CMAKE_CXX_COMPILER_VERSION}")
    find_program(LLVM_CONFIG
            NAMES "llvm-config-${_clang_major}" "llvm-config"
            DOC "Path to llvm-config"
            REQUIRED)
endif ()

execute_process(
        COMMAND ${LLVM_CONFIG} --cmakedir
        OUTPUT_VARIABLE LLVM_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "LLVM_DIR ............... ${LLVM_DIR}")

if (NOT EXISTS ${LLVM_DIR})
    message(STATUS "LLVM cmakedir is specified and missing: ${LLVM_DIR}")
    execute_process(
            COMMAND ${LLVM_CONFIG} --version
            OUTPUT_VARIABLE LLVM_PACKAGE_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND ${LLVM_CONFIG} --includedir
            OUTPUT_VARIABLE LLVM_INCLUDE_DIRS
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND ${LLVM_CONFIG} --libdir
            OUTPUT_VARIABLE LLVM_LIBRARY_DIRS
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(LLVM_DEFINITIONS)
    set(LLVM_ENABLE_ASSERTIONS OFF)
    set(LLVM_ENABLE_EH OFF)
    execute_process(
            COMMAND ${LLVM_CONFIG} --has-rtti
            OUTPUT_VARIABLE LLVM_ENABLE_RTTI_RAW
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(LLVM_ENABLE_RTTI OFF)
    message(STATUS "llvm-config --has-rtti : ${LLVM_ENABLE_RTTI_RAW}")
    if ("${LLVM_ENABLE_RTTI_RAW}" STREQUAL "YES")
        set(LLVM_ENABLE_RTTI ON)
    endif ()
    set(LLVM_ENABLE_THREADS OFF)
    execute_process(
            COMMAND ${LLVM_CONFIG} --bindir
            OUTPUT_VARIABLE LLVM_TOOLS_BINARY_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else ()
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    find_package(LLVM REQUIRED CONFIG)
endif ()

message(STATUS "Found LLVM ............. ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_INCLUDE_DIRS: ..... ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM_LIBRARY_DIRS: ..... ${LLVM_LIBRARY_DIRS}")
message(STATUS "LLVM_DEFINITIONS: ...... ${LLVM_DEFINITIONS}")
message(STATUS "LLVM_ENABLE_ASSERTIONS:  ${LLVM_ENABLE_ASSERTIONS}")
message(STATUS "LLVM_ENABLE_EH: ........ ${LLVM_ENABLE_EH}")
message(STATUS "LLVM_ENABLE_RTTI: ...... ${LLVM_ENABLE_RTTI}")
message(STATUS "LLVM_ENABLE_THREADS: ... ${LLVM_ENABLE_THREADS}")
message(STATUS "LLVM_TOOLS_BINARY_DIR: . ${LLVM_TOOLS_BINARY_DIR}")
message(STATUS "C++ include path ....... ${LLVM_INCLUDE_DIRS}/c++/v1/")

add_library(toolchain INTERFACE)
add_library(toolchain::toolchain ALIAS toolchain)

target_compile_definitions(toolchain INTERFACE "${LLVM_DEFINITIONS}")

# libc++ from clang 14 on, unless the target cannot have it. A sysroot with no
# libc++ headers is one: the flatpak SDK, for one, ships only GCC's libstdc++,
# and its runtime has no libc++.so, so a libc++ build neither compiles against
# it nor runs on it. The host LLVM's c++/v1 is this machine's, not the
# target's, so it is no substitute.
set(IVI_CLANG_STDLIB "auto" CACHE STRING
        "C++ standard library for a clang build: auto, libc++ or libstdc++")
set_property(CACHE IVI_CLANG_STDLIB PROPERTY STRINGS auto libc++ libstdc++)
if (IVI_CLANG_STDLIB STREQUAL "auto")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        set(STDLIB libstdc++)
    elseif (CMAKE_SYSROOT AND NOT EXISTS "${CMAKE_SYSROOT}/usr/include/c++/v1")
        message(STATUS "no libc++ in ${CMAKE_SYSROOT}; using libstdc++")
        set(STDLIB libstdc++)
    else ()
        set(STDLIB libc++)
    endif ()
elseif (IVI_CLANG_STDLIB MATCHES "^(libc\\+\\+|libstdc\\+\\+)$")
    set(STDLIB ${IVI_CLANG_STDLIB})
else ()
    message(FATAL_ERROR
            "IVI_CLANG_STDLIB=${IVI_CLANG_STDLIB}: expected auto, libc++ or libstdc++")
endif ()
message(STATUS "stdlib ................. ${STDLIB}")

target_compile_options(toolchain INTERFACE -stdlib=${STDLIB})
if (STDLIB STREQUAL "libc++")
    target_compile_options(toolchain INTERFACE -isystem"${LLVM_INCLUDE_DIRS}/c++/v1/")
endif ()

if (ENABLE_STATIC_LINK)
    if (STDLIB STREQUAL "libc++")
        # -Wl,-Bstatic,-lc++,-lc++abi,-Bdynamic does not link statically: the
        # driver appends its own -lc++ after all user options, where the restored
        # -Bdynamic applies to it. -Wl,--as-needed hides that in Release.
        # -static-libstdc++ would work, but this file bans it with -static-libgcc.
        # -stdlib= is not repeated here; -nostdlib++ leaves it nothing to select.
        target_link_options(toolchain INTERFACE
            -nostdlib++ -l:libc++.a -l:libc++abi.a
            -lc -fuse-ld=lld -v)
    else ()
        target_link_options(toolchain INTERFACE -static-libstdc++ -static-libgcc -stdlib=${STDLIB} -lc -fuse-ld=lld -v)
    endif ()
else ()
    target_link_options(toolchain INTERFACE -stdlib=${STDLIB} -lc -fuse-ld=lld -v)
endif ()

if (ENABLE_LTO AND IPO_SUPPORT_RESULT)
    set_property(TARGET toolchain PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
