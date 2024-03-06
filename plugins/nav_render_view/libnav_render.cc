/*
 * Copyright 2020-2024 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "libnav_render.h"

#include <plugins/common/common.h>

#include <dlfcn.h>

namespace nav_render_view_plugin {

constexpr char kNaviRenderSoName[] = "libnav_render.so";

LibNavRenderExports::LibNavRenderExports(void* lib) {
  if (lib != nullptr) {
    GetFuncAddress(lib, "nav_render_version", &GetInterfaceVersion);
    GetFuncAddress(lib, "nav_render_initialize", &Initialize);
    GetFuncAddress(lib, "nav_render_initialize2", &Initialize2);
    GetFuncAddress(lib, "nav_render_de_initialize", &DeInitialize);
    GetFuncAddress(lib, "nav_render_run_task", &RunTask);
    GetFuncAddress(lib, "nav_render_render", &Render);
    GetFuncAddress(lib, "nav_render_render2", &Render2);
    GetFuncAddress(lib, "nav_render_resize", &Resize);
  }
}

LibNavRenderExports* LibNavRender::operator->() {
  return loadExports();
}

LibNavRenderExports* LibNavRender::loadExports() {
  static LibNavRenderExports exports = [] {
    void* lib;

    if (GetProcAddress(RTLD_DEFAULT,
                       "nav_render_initialize2"))  // Search the global scope
                                                   // for pre-loaded library.
    {
      lib = RTLD_DEFAULT;
    } else {
      lib = dlopen(kNaviRenderSoName, RTLD_LAZY | RTLD_LOCAL);
    }

    return LibNavRenderExports(lib);
  }();

  return exports.Initialize2 ? &exports : nullptr;
}

class LibNavRender LibNavRender;

}  // namespace nav_render_view_plugin
