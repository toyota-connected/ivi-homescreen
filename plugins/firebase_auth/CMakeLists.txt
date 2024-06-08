#
# Copyright 2023 Toyota Connected North America
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

set(CMAKE_THREAD_PREFER_PTHREAD ON)
include(FindThreads)

add_library(plugin_firebase_auth STATIC
        firebase_auth_plugin.cc
        firebase_auth_plugin_c_api.cc
        messages.g.cc
)

target_compile_definitions(plugin_firebase_auth PRIVATE INTERNAL_EXPERIMENTAL)

target_include_directories(plugin_firebase_auth PUBLIC
        include
)

target_link_directories(plugin_firebase_auth PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(plugin_firebase_auth PUBLIC
        plugin_firebase_core
        firebase_sdk
        flutter
        platform_homescreen
)
