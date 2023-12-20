#
# Copyright 2023 Toyota Connected North America
# @copyright Copyright (c) 2022 Woven Alpha, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include_guard()

function(COMPILER_FLAGS_APPEND scope add_val conflict_match)
    if ((NOT "${conflict_match}" STREQUAL "") AND
    ((${CMAKE_C_COMPILER_ARG1} MATCHES ${conflict_match}) OR
    (${CMAKE_CXX_COMPILER_ARG1} MATCHES ${conflict_match}) OR
    ($ENV{CFLAGS} MATCHES ${conflict_match}) OR
    ($ENV{CXXFLAGS} MATCHES ${conflict_match})))
        message("-- IGNORE APPEND FLAGS .... ${add_val}")
        return()
    endif ()

    if (${scope} STREQUAL "RELEASE")
        string(APPEND CMAKE_C_FLAGS_RELEASE "${add_val}")
        string(APPEND CMAKE_CXX_FLAGS_RELEASE "${add_val}")
        message("-- APPEND RELEASE FLAGS ... ${add_val}")
    elseif (${scope} STREQUAL "DEBUG")
        string(APPEND CMAKE_C_FLAGS_DEBUG "${add_val}")
        string(APPEND CMAKE_CXX_FLAGS_DEBUG "${add_val}")
        message("-- APPEND DEBUG FLAGS ..... ${add_val}")
    else ()
        string(APPEND CMAKE_C_FLAGS "${add_val}")
        string(APPEND CMAKE_CXX_FLAGS "${add_val}")
        message("-- APPEND FLAGS ........... ${add_val}")
    endif ()
endfunction(COMPILER_FLAGS_APPEND)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_options(
        -Wtrigraphs
        -Wchar-subscripts
        -Wcomment
        -Wreturn-type
        -Wsequence-point
        -Wswitch
        -Wuninitialized
        -Wunused
        -Wswitch-bool
        -Wformat
        -Wformat-security
        -Wconversion
        -Wcast-align
        #-Wcast-qual
        $<$<COMPILE_LANGUAGE:CXX>:-Wunused-parameter>
        $<$<COMPILE_LANGUAGE:CXX>:-Winvalid-offsetof>
        $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
)

add_compile_options(
        -fsigned-char
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:Debug>:-g>
)

add_compile_definitions(
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

COMPILER_FLAGS_APPEND(RELEASE " -fstack-protector-all" "-f(no-)?stack-protector(-all|-strong)?")
COMPILER_FLAGS_APPEND(RELEASE " -fno-omit-frame-pointer" "-f(no-)?omit-frame-pointer")
COMPILER_FLAGS_APPEND(RELEASE " -Wformat=2" "-Wformat(=[0-9]+)?")
COMPILER_FLAGS_APPEND(RELEASE " -D_FORTIFY_SOURCE=2" "-D_FORTIFY_SOURCE(=[0-9]+)?")

if (BUILD_PLUGIN_FIREBASE_CORE OR
        BUILD_PLUGIN_CLOUD_FIRESTORE OR
        BUILD_PLUGIN_FIREBASE_AUTH OR
        BUILD_PLUGIN_FIREBASE_STORAGE OR
        BUILD_CRASH_HANDLER OR
        BUILD_PLUGIN_FILAMENT_VIEW)
    string(APPEND CMAKE_CXX_FLAGS " -frtti")
else ()
    string(APPEND CMAKE_CXX_FLAGS " -fno-rtti")
endif ()

string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--build-id=sha1")

string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,--no-undefined")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,--gc-sections")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,--as-needed")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,nodlopen")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,noexecstack")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,relro -Wl,-z,now")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -pie")

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(ENV64BIT)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_compile_definitions(ENV32BIT)
endif ()

message("-- CC ..................... ${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}")
message("-- CXX .................... ${CMAKE_C_COMPILER} ${CMAKE_CXX_COMPILER_ARG1}")
message("-- CFLAGS ................. ${CMAKE_C_FLAGS}")
message("-- CXXFLAGS ............... ${CMAKE_CXX_FLAGS}")
