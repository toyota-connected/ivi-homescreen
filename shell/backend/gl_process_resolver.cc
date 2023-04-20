/*
 * Copyright 2020-2022 Toyota Connected North America
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

#include "gl_process_resolver.h"

#include <dlfcn.h>
#include <cassert>

#include "logging.h"

std::shared_ptr<EglProcessResolver> GlProcessResolver::sInstance = nullptr;

EglProcessResolver::~EglProcessResolver() {
  for (auto& item : m_handles) {
    dlclose(item);
  }
}

int EglProcessResolver::GetHandle(std::array<char[kSoMaxLength], kSoCount> arr,
                                  void** out_handle) {
  void* handle = nullptr;

  for (const auto& item : arr) {
    handle = dlopen(item, RTLD_LAZY | RTLD_LOCAL);
    if (handle) {
      DLOG(INFO) << "dlopen: " << item;
      break;
    }
  }

  if (handle == nullptr) {
    return -1;
  }

  *out_handle = handle;

  return 1;
}

void EglProcessResolver::Initialize() {
  void* handle;
  for (const auto& soNames : kGlSoNames) {
    GetHandle(soNames, &handle);
    if (handle) {
      m_handles.push_back(handle);
    } else {
      LOG(ERROR) << soNames[0] << ": Library not found";
      assert(false);
    }
  }
}

void* EglProcessResolver::process_resolver(const char* name) {
  if (name == nullptr) {
    LOG(ERROR) << "gl_proc_resolver for nullptr; ignoring";
    return nullptr;
  }

  void* address;

  for (auto& item : m_handles) {
    address = dlsym(item, name);
    if (address)
      return address;
  }

  address = reinterpret_cast<void*>(eglGetProcAddress(name));
  if (address) {
    return address;
  }

  LOG(ERROR) << "gl_proc_resolver: could not resolve symbol \"" << name
                 << "\"";

  return nullptr;
}
