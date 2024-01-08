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

add_library(plugin_cloud_firestore STATIC
        cloud_firestore_plugin_c_api.cc
        cloud_firestore_plugin.cc
        firestore_codec.cc
        messages.g.cc
)

target_compile_definitions(plugin_cloud_firestore PUBLIC ENABLE_PLUGIN_CLOUD_FIRESTORE)

target_compile_definitions(plugin_cloud_firestore PRIVATE INTERNAL_EXPERIMENTAL RAPIDJSON_HAS_STDSTRING=1)

target_include_directories(plugin_cloud_firestore PUBLIC include)

target_link_libraries(plugin_cloud_firestore PUBLIC
        plugin_firebase_core
        firebase_sdk
        flutter
        platform_homescreen
)

add_sanitizers(plugin_cloud_firestore)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_cloud_firestore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
