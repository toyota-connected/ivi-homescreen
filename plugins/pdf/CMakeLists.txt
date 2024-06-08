#
# Copyright 2024 Toyota Connected North America
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

if (NOT EXISTS ${PDFIUM_INCLUDE_DIR})
    message(FATAL_ERROR "${PDFIUM_INCLUDE_DIR} does not exist")
endif ()
if (NOT EXISTS ${PDFIUM_LINK_LIBRARIES_DIR})
    message(FATAL_ERROR "${PDFIUM_LINK_LIBRARIES_DIR} does not exist")
endif ()

add_library(plugin_pdf STATIC
        pdf_plugin_c_api.cc
        pdf_plugin.cc
        messages.cc
)

target_include_directories(plugin_pdf PRIVATE
        include
        ${PDFIUM_INCLUDE_DIR}

)

target_link_directories(plugin_pdf PUBLIC
        ${PDFIUM_LINK_LIBRARIES_DIR}
)

target_link_libraries(plugin_pdf PUBLIC
        flutter
        platform_homescreen
        plugin_common
        pdfium
)
