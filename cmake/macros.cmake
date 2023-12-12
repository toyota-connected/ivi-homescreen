#
# Copyright 2020-2022 Toyota Connected North America
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

macro(ENABLE_TEXTURE texture)

    list(APPEND TEXTURES ${texture})

    string(TOUPPER ${texture} ucase_texture)

    target_compile_definitions(homescreen PRIVATE ENABLE_TEXTURE_${ucase_texture})

    target_sources(homescreen PRIVATE textures/${texture}/texture_${texture}.cc)

endmacro(ENABLE_TEXTURE)

macro(ENABLE_PLUGIN plugin)

    #list(APPEND PLUGINS ${plugin})

    string(TOUPPER ${plugin} ucase_plugin)

    # target_compile_definitions(homescreen PRIVATE ENABLE_PLUGIN_${ucase_plugin})

    # target_sources(homescreen PRIVATE plugins/${plugin}/${plugin}.cc)

endmacro(ENABLE_PLUGIN)
