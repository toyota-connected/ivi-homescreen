#
# Copyright 2020 Toyota Connected North America
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

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

string(APPEND CMAKE_ASM_FLAGS " -fno-integrated-as")

string(APPEND CMAKE_CXX_FLAGS " -fno-rtti")

string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--no-undefined")
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-fvisibility=hidden")
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--build-id=sha1")
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--gc-sections")
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--as-needed")

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(ENV64BIT)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_compile_definitions(ENV32BIT)
endif ()

set(CMAKE_THREAD_PREFER_PTHREAD ON)
include(FindThreads)

include(wayland)

set(SRC_FILES
        main.cc
        app.cc
        display.cc
        egl.cc
        egl_window.cc
        gl_resolver.cc
        engine.cc
        platform_channel.cc

        textures/texture.cc

        ../third_party/flutter/shell/platform/common/client_wrapper/core_implementations.cc
        #../third_party/flutter/shell/platform/common/client_wrapper/plugin_registrar.cc
        ../third_party/flutter/shell/platform/common/client_wrapper/standard_codec.cc
        ../third_party/flutter/shell/platform/common/json_message_codec.cc
        ../third_party/flutter/shell/platform/common/json_method_codec.cc
        ../third_party/flutter/shell/platform/common/path_utils.cc
        #../third_party/flutter/shell/platform/common/text_editing_delta.cc
        ../third_party/flutter/shell/platform/common/text_input_model.cc

        ${WAYLAND_PROTOCOL_SOURCES}

        ../third_party/flutter/fml/platform/linux/paths_linux.cc

        ../third_party/flutter/fml/platform/posix/file_posix.cc
        ../third_party/flutter/fml/platform/posix/paths_posix.cc
        ../third_party/flutter/fml/platform/posix/posix_wrappers_posix.cc
        ../third_party/flutter/fml/platform/posix/shared_mutex_posix.cc

        ../third_party/flutter/fml/command_line.cc
        ../third_party/flutter/fml/file.cc
        ../third_party/flutter/fml/log_settings.cc
        ../third_party/flutter/fml/log_settings_state.cc
        ../third_party/flutter/fml/logging.cc
        ../third_party/flutter/fml/paths.cc
        ../third_party/flutter/fml/unique_fd.cc
        )

add_executable(homescreen ${SRC_FILES})

include(plugins)
include(textures)

target_compile_definitions(homescreen
        PRIVATE
        HAVE_STRCHRNUL
        EGL_NO_X11
        MESA_EGL_NO_X11_HEADERS
        LINUX
        WL_EGL_PLATFORM
        HAVE_MEMFD_CREATE
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
        )

include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GLIB2_INCLUDE_DIRS}
        ..
        ../third_party
        ../third_party/flutter
        ../third_party/flutter/shell/platform/common/public
        ../third_party/flutter/shell/platform/common/client_wrapper
        ../third_party/rapidjson/include
)

target_link_libraries(homescreen PRIVATE
        ${WAYLAND_LIBRARIES}
        EGL GLESv2
        Threads::Threads
        dl
        )

target_link_directories(homescreen PRIVATE
        ${CMAKE_BINARY_DIR}
        )

cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_support_result OUTPUT ipo_support_output)
if (ipo_support_result)
    set_property(TARGET homescreen PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(WARNING "IPO is not supported: ${ipo_support_output}")
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(homescreen PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++ -I${CMAKE_SYSROOT}/include/c++/v1>)
    target_link_options(homescreen PRIVATE
            -fuse-ld=lld -lc++ -lc -lm -v)
endif ()

install(TARGETS homescreen DESTINATION bin)
