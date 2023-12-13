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

add_library(plugin_url_launcher STATIC
        url_launcher_plugin_c_api.cc
        url_launcher_plugin.cc
        messages.g.cc
)

target_compile_definitions(plugin_url_launcher PUBLIC -DENABLE_PLUGIN_URL_LAUNCHER)

target_include_directories(plugin_url_launcher PRIVATE include)

target_link_libraries(plugin_url_launcher PUBLIC
        flutter_common
        platform_homescreen
)

add_sanitizers(plugin_url_launcher)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_url_launcher PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
