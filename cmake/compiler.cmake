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
        -Wcast-qual
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

string(APPEND CMAKE_C_FLAGS_RELEASE " -fstack-protector-all")
string(APPEND CMAKE_C_FLAGS_RELEASE " -fno-omit-frame-pointer")
string(APPEND CMAKE_C_FLAGS_RELEASE " -Wformat=2")
string(APPEND CMAKE_C_FLAGS_RELEASE " -D_FORTIFY_SOURCE=2")

string(APPEND CMAKE_CXX_FLAGS " -fno-rtti")

string(APPEND CMAKE_CXX_FLAGS_RELEASE " -fstack-protector-all")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -fno-omit-frame-pointer")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -Wformat=2")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -D_FORTIFY_SOURCE=2")

string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--build-id=sha1")

string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,--no-undefined")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,--gc-sections")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,--as-needed")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,nodlopen -Wl,-z,nodump")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,noexecstack")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,relro -Wl,-z,now")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -pie")

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(ENV64BIT)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_compile_definitions(ENV32BIT)
endif ()
