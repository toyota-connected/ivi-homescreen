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

set(CPACK_GENERATOR "TGZ;RPM;DEB")

set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "Toyota Connected North America")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "IVI Homescreen - ${CMAKE_BUILD_TYPE} ${CHANNEL}")
set(PACKAGE_FILE_NAME ${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR})
string(TOLOWER ${PACKAGE_FILE_NAME} CPACK_PACKAGE_FILE_NAME)
set(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Joel Winarske")
set(CPACK_DEBIAN_COMPRESSION_TYPE "lzma")

if(${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set(CPACK_STRIP_FILES homescreen)
endif()

include(CPack)
