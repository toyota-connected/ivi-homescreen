/*
 * Copyright 2020 Toyota Connected North America
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

#include <unistd.h>
#include <vector>

class GlResolver {
 public:
  GlResolver();
  ~GlResolver();
  GlResolver(const GlResolver&) = delete;
  const GlResolver& operator=(const GlResolver&) = delete;

  void* gl_process_resolver(const char* name);

 private:
  [[maybe_unused]]
  size_t m_so_count;
  std::vector<void*> m_hDL;
  static int get_handle(std::array<char[kSoMaxLength], kSoCount> arr,
                        void** out_handle);
};
