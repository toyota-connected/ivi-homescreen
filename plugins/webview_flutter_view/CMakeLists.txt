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

add_library(plugin_webview_flutter_view STATIC
        webview_flutter_view_plugin_c_api.cc
        webview_flutter_view_plugin.cc
        messages.g.cc
)

target_compile_definitions(plugin_webview_flutter_view PUBLIC ENABLE_PLUGIN_WEBVIEW_FLUTTER_VIEW)

target_include_directories(plugin_webview_flutter_view PRIVATE include)

target_link_libraries(plugin_webview_flutter_view PUBLIC
        flutter
        platform_homescreen
)
