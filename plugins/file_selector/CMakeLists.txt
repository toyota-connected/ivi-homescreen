#
# Copyright 2023-2024 Toyota Connected North America
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

add_library(plugin_file_selector STATIC
        file_selector_plugin_c_api.cc
        file_selector_plugin.cc
        messages.cc
)

target_compile_definitions(plugin_file_selector PUBLIC ENABLE_PLUGIN_FILE_SELECTOR)

target_include_directories(plugin_file_selector PRIVATE include)

target_link_libraries(plugin_file_selector PUBLIC
        flutter
        platform_homescreen
)
