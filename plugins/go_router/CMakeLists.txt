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

add_library(plugin_go_router STATIC
        go_router_plugin_c_api.cc
        go_router_plugin.cc
        messages.cc
)

target_include_directories(plugin_go_router PRIVATE include)

target_link_libraries(plugin_go_router PUBLIC
        flutter
        platform_homescreen
)

add_sanitizers(plugin_go_router)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_go_router PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
