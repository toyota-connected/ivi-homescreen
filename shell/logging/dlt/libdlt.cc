/*
 * Copyright 2023 Toyota Connected North America
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

#include "libdlt.h"

#include "../../shared_library.h"

#include <dlfcn.h>

LibDltExports::LibDltExports(void* lib) {
  if (lib != nullptr) {
    GetFuncAddress(lib, "dlt_check_library_version", &CheckLibraryVersion);
    GetFuncAddress(lib, "dlt_register_app", &RegisterApp);
    GetFuncAddress(lib, "dlt_unregister_app", &UnregisterApp);
    GetFuncAddress(lib, "dlt_register_context", &RegisterContext);
    GetFuncAddress(lib, "dlt_unregister_context", &UnregisterContext);
    GetFuncAddress(lib, "dlt_user_log_write_start", &UserLogWriteStart);
    GetFuncAddress(lib, "dlt_user_log_write_finish", &UserLogWriteFinish);
    GetFuncAddress(lib, "dlt_user_log_write_string", &UserLogWriteString);
    GetFuncAddress(lib, "dlt_user_log_write_int", &UserLogWriteInt);
    GetFuncAddress(lib, "dlt_user_log_write_int8", &UserLogWriteInt8);
    GetFuncAddress(lib, "dlt_user_log_write_int16", &UserLogWriteInt16);
    GetFuncAddress(lib, "dlt_user_log_write_int32", &UserLogWriteInt32);
    GetFuncAddress(lib, "dlt_user_log_write_int64", &UserLogWriteInt64);
  }
}

LibDltExports* LibDlt::operator->() {
  return loadExports();
}

LibDltExports* LibDlt::loadExports() {
  static LibDltExports exports = [] {
    void* lib;

    if (GetProcAddress(RTLD_DEFAULT,
                       "dlt_user_log_write_start"))  // Search the global scope
                                                     // for pre-loaded library.
    {
      lib = RTLD_DEFAULT;
    } else {
      lib = dlopen("libdlt.so.2", RTLD_LAZY | RTLD_LOCAL);
    }

    return LibDltExports(lib);
  }();

  return exports.UserLogWriteStart ? &exports : nullptr;
}

class LibDlt LibDlt;
