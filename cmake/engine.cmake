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

set_property(GLOBAL APPEND PROPERTY PROJECT_TARGETS flutter_engine::flutter_engine)
add_library(flutter_engine::flutter_engine SHARED IMPORTED GLOBAL)

if (NOT CMAKE_CROSSCOMPILING)

    if (NOT FLUTTER_EMBEDDER_HEADER)

        include(FetchContent)

        if (NOT FLUTTER_ENGINE_SHA)

            if (NOT CHANNEL)
                set(CHANNEL "beta" CACHE STRING "The flutter channel to be used for downloading the flutter_embedder.h header file. Choose: master, dev, beta, stable" FORCE)
                message(STATUS "Flutter Channel not set, defaulting to beta")
            endif ()

            message(STATUS "Flutter Channel ........ ${CHANNEL}")

            FetchContent_Declare(engine-version
                    URL https://raw.githubusercontent.com/flutter/flutter/${CHANNEL}/bin/internal/engine.version
                    DOWNLOAD_NAME engine.version
                    DOWNLOAD_NO_EXTRACT TRUE
                    DOWNLOAD_DIR ${CMAKE_BINARY_DIR}
                    )

            FetchContent_GetProperties(engine-version)
            if (NOT engine-version_POPULATED)
                FetchContent_Populate(engine-version)
                file(READ ${CMAKE_BINARY_DIR}/engine.version FLUTTER_ENGINE_SHA)
                string(REPLACE "\n" "" FLUTTER_ENGINE_SHA ${FLUTTER_ENGINE_SHA})
            else ()
                MESSAGE(FATAL "Unable to determine engine-version, please override FLUTTER_ENGINE_SHA")
            endif ()

        endif ()

        message(STATUS "Engine SHA1 ............ ${FLUTTER_ENGINE_SHA}")

        # Download and setup the Flutter Engine.
        set(FLUTTER_EMBEDDER_ARTIFACTS_ZIP flutter_embedder_${FLUTTER_ENGINE_SHA}.zip)
        set(FLUTTER_BUCKET_BASE "https://storage.googleapis.com/flutter_infra_release/flutter")

        if (NOT EXISTS ${FLUTTER_EMBEDDER_ARTIFACTS_ZIP})
            file(DOWNLOAD
                    ${FLUTTER_BUCKET_BASE}/${FLUTTER_ENGINE_SHA}/linux-x64/linux-x64-embedder
                    ${CMAKE_BINARY_DIR}/${FLUTTER_EMBEDDER_ARTIFACTS_ZIP}
                    SHOW_PROGRESS
                    )
            execute_process(
                    COMMAND ${CMAKE_COMMAND} -E tar xzf ${FLUTTER_EMBEDDER_ARTIFACTS_ZIP}
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            )
        endif ()

        set(FLUTTER_ENGINE_LIBRARY ${CMAKE_BINARY_DIR}/libflutter_engine.so)
        set(ENGINE_INCLUDE_DIRECTORIES ${CMAKE_BINARY_DIR})
    else ()
        message(STATUS "Engine ................. ${FLUTTER_ENGINE_LIBRARY}")
        message(STATUS "Engine ................. ${ENGINE_INCLUDE_DIRECTORIES}")
    endif ()

else ()
    set(ENGINE_INCLUDE_DIRECTORIES ${CMAKE_SYSROOT}/include)
    set(FLUTTER_ENGINE_LIBRARY ${CMAKE_SYSROOT}/lib/libflutter_engine.so)
endif ()

set_target_properties(flutter_engine::flutter_engine
        PROPERTIES
        LABELS LIBRARY
        INTERFACE_INCLUDE_DIRECTORIES ${ENGINE_INCLUDE_DIRECTORIES}
        IMPORTED_LOCATION ${FLUTTER_ENGINE_LIBRARY}
        )
