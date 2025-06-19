include_guard()

cmake_policy(SET CMP0075 NEW)

if (NOT LLVM_CONFIG)
    find_program(LLVM_CONFIG "llvm-config" DOC "Path to llvm-config" REQUIRED)
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
    if (${LLVM_ENABLE_RTTI_RAW} STREQUAL "YES")
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

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
    message(STATUS "stdlib ................. libc++")
    set(STDLIB libc++)
else ()
    message(STATUS "stdlib ................. libstdc++")
    set(STDLIB libstdc++)
endif ()

target_compile_options(toolchain INTERFACE -stdlib=${STDLIB} -isystem"${LLVM_INCLUDE_DIRS}/c++/v1/")

if (ENABLE_STATIC_LINK)
    target_link_options(toolchain INTERFACE -static-libstdc++ -static-libgcc -stdlib=${STDLIB} -lc -fuse-ld=lld -v)
else ()
    target_link_options(toolchain INTERFACE -stdlib=${STDLIB} -lc -fuse-ld=lld -v)
endif ()

if (ENABLE_LTO AND IPO_SUPPORT_RESULT)
    set_property(TARGET toolchain PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
