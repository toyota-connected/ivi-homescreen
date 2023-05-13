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

#pragma once

#include <dlfcn.h>
#include <cassert>
#include <iostream>

/**
 * @brief Dynamic Symbol Resolver
 * @param[in] library The library to look for symbol in
 * @param[in] name symbol name
 * @return void*
 * @retval pointer to symbol
 * @relation
 * internal
 */
inline void* GetProcAddress(void* library, const char* name) {
  void* symbol = dlsym(library, name);

  if (!symbol && library != RTLD_DEFAULT) {
    std::cerr << "GetProcAddress: " << name << " not found!" << std::endl;
    const char* reason = dlerror();
    (void)reason;
  }

  return symbol;
}

/**
 * @brief Function Pointer Resolver
 * @param[in] library The library to look for symbol in
 * @param[in] function_name symbol name to look for
 * @param[out] out FunctionPointer of symbol
 * @return void
 * @relation
 * internal
 */
template <typename FunctionPointer>
inline void GetFuncAddress(void* library,
                           const char* function_name,
                           FunctionPointer* out) {
  auto symbol = dlsym(library, function_name);
  if (!symbol) {
    std::cerr << "GetFuncAddress: " << function_name << " not found!"
              << std::endl;
    const char* reason = dlerror();
    (void)reason;
  }
  *out = reinterpret_cast<FunctionPointer>(symbol);
}
