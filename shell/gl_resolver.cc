// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <EGL/egl.h>
#include <dlfcn.h>
#include <flutter/fml/logging.h>
#include <cassert>

#include "constants.h"
#include "gl_resolver.h"

int GlResolver::get_handle(std::array<char[kSoMaxLength], kSoCount> arr,
                           void** out_handle) {
  void* handle = nullptr;

  for (const auto& item : arr) {
    handle = dlopen(item, RTLD_LAZY | RTLD_LOCAL);
    if (handle) {
      FML_DLOG(INFO) << "dlopen: " << item;
      break;
    }
  }

  if (handle == nullptr) {
    return -1;
  }

  *out_handle = handle;

  return 1;
}

GlResolver::GlResolver() : m_so_count(kGlSoNames->size()) {
  void* handle;
  for (const auto& soNames : kGlSoNames) {
    get_handle(soNames, &handle);
    if (handle) {
      m_hDL.push_back(handle);
    } else {
      FML_LOG(ERROR) << soNames[0] << ": Library not found";
      assert(false);
    }
  }
}

GlResolver::~GlResolver() {
  for (auto& item : m_hDL) {
    dlclose(item);
  }
}

void* GlResolver::gl_process_resolver(const char* name) {
  if (name == nullptr) {
    FML_LOG(ERROR) << "gl_proc_resolver for nullptr; ignoring";
    return nullptr;
  }

  void* address;

  for (auto& item : m_hDL) {
    address = dlsym(item, name);
    if (address) {
      return address;
    }
  }

  address = reinterpret_cast<void*>(eglGetProcAddress(name));
  if (address) {
    return address;
  }

  FML_LOG(ERROR) << "gl_proc_resolver: could not resolve symbol \"" << name
                 << "\"";

  return nullptr;
}
