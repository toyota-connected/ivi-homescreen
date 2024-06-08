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

pkg_check_modules(SECRET IMPORTED_TARGET REQUIRED libsecret-1)

add_library(plugin_secure_storage STATIC
        secure_storage_plugin_c_api.cc
        secure_storage_plugin.cc
        messages.cc
)

target_include_directories(plugin_secure_storage PRIVATE include)

target_link_libraries(plugin_secure_storage PUBLIC
        flutter
        platform_homescreen
        PkgConfig::SECRET
)
