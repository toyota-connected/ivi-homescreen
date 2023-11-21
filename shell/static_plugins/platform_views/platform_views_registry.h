/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include <map>

class PlatformView;

class PlatformViewsRegistry {
 public:
  PlatformViewsRegistry();

  ~PlatformViewsRegistry();

  void AddPlatformView(int32_t id, std::unique_ptr<PlatformView> platform_view);

  void RemovePlatformView(int32_t id);

  PlatformView* GetPlatformView(int32_t id);

  PlatformViewsRegistry(PlatformViewsRegistry& other) = delete;

  void operator=(const PlatformViewsRegistry&) = delete;

  /**
   * @brief Get instance of PlatformViewsRegistry class
   * @return PlatformViewsRegistry&
   * @retval Instance of the PlatformViewsRegistry class
   * @relation
   * internal
   */
  static PlatformViewsRegistry& GetRegistry() {
    if (!sRegistry) {
      sRegistry = std::make_shared<PlatformViewsRegistry>();
    }
    return *sRegistry;
  }

 private:
  std::map<int32_t, std::unique_ptr<PlatformView>> registry_;

 protected:
  static std::shared_ptr<PlatformViewsRegistry> sRegistry;
};
