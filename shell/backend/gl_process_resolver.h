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

#include <string>
#include <vector>

class EglProcessResolver {
 public:
  static constexpr char kGlSoNames[2UL][15UL] = {{"libGLESv2.so.2"},
                                                 {"libEGL.so.1"}};

  // Non-copyable, non-movable. A copy's destructor would tear down state
  // it does not own; capture GetInstance() by reference only.
  EglProcessResolver(const EglProcessResolver&) = delete;
  EglProcessResolver& operator=(const EglProcessResolver&) = delete;
  EglProcessResolver(EglProcessResolver&&) = delete;
  EglProcessResolver& operator=(EglProcessResolver&&) = delete;

  /**
   * @brief Get DLL handle
   * @param[in] lib name of library
   * @param[out] out_handle DLL handle; set to nullptr on failure
   * @return int
   * @retval 1 Normal end
   * @retval -1 Abnormal end
   * @relation
   * internal
   */
  static int GetHandle(const std::string& lib, void** out_handle);

  /**
   * @brief Resolve the process
   * @param[in] name Process name
   * @return void*
   * @retval Process address
   * @relation
   * wayland
   */
  void* process_resolver(const char* name) const;

 private:
  friend class GlProcessResolver;

  /**
   * @brief Loads the GL client libraries. Runs exactly once, under the
   *        block-scope static initialization guarantee in
   *        GlProcessResolver::GetInstance().
   * @relation
   * internal
   */
  EglProcessResolver();

  /**
   * Handles are intentionally never dlclose()d. The resolver lives for the
   * lifetime of the process, and unloading GL client libraries during
   * static destruction is unsafe: driver worker threads and TLS
   * destructors may still reference their text segments.
   */
  ~EglProcessResolver() = default;

  std::vector<std::pair<void*, std::string>> m_handles;
};

class GlProcessResolver {
 public:
  GlProcessResolver(GlProcessResolver& other) = delete;

  void operator=(const GlProcessResolver&) = delete;

  /**
   * @brief Get instance of EglProcessResolver class
   * @return EglProcessResolver&
   * @retval Instance of the EglProcessResolver class
   * @relation
   * internal
   *
   * Thread-safe: initialization of a block-scope variable with static
   * storage duration is serialized by the implementation
   * (C++11 [stmt.dcl]p4). Concurrent first callers block until
   * construction completes; exactly one instance is ever created.
   */
  static EglProcessResolver& GetInstance() {
    static EglProcessResolver instance;
    return instance;
  }
};
