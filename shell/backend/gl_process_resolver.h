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

#pragma once

#include <memory>
#include <vector>

#include "constants.h"

class EglProcessResolver {
 public:
  static constexpr size_t kSoCount = 2UL;
  static constexpr size_t kSoMaxLength = 15;
  static constexpr std::array<char[kSoMaxLength], kSoCount> kGlSoNames[]{
      {"libEGL.so.1", "libEGL.so"},
      {"libGLESv2.so.2", "libGLESv2"},
  };

  ~EglProcessResolver();

  void Initialize();

  static int GetHandle(std::array<char[kSoMaxLength], kSoCount> arr,
                       void** out_handle);

  void* process_resolver(const char* name);

 private:
  std::vector<void*> m_handles;
};

class GlProcessResolver {
 public:
  GlProcessResolver(GlProcessResolver& other) = delete;

  void operator=(const GlProcessResolver&) = delete;

  static EglProcessResolver& GetInstance() {
    if (!sInstance) {
      sInstance = std::make_shared<EglProcessResolver>();
      sInstance->Initialize();
    }
    return *sInstance;
  }

 protected:
  static std::shared_ptr<EglProcessResolver> sInstance;
};
