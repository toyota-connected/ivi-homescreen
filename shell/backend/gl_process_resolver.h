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

class EglProcessResolver {
 public:
  static constexpr char kGlSoNames[2UL][15UL] = {{"libGLESv2.so.2"},
                                                 {"libEGL.so.1"}};

  ~EglProcessResolver();

  /**
   * @brief Initialize
   * @return void
   * @relation
   * internal
   */
  void Initialize();

  /**
   * @brief Get DLL handle
   * @param[in] lib of library
   * @param[out] out_handle DLL handle
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
   */
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
