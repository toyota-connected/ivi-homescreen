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

#include <memory>
#include <mutex>

#include "platform_view.h"
#include "platform_views_registry.h"

std::shared_ptr<PlatformViewsRegistry> PlatformViewsRegistry::sInstance =
    nullptr;

std::mutex gPlatformViewsRegistryMutex;

PlatformViewsRegistry::PlatformViewsRegistry() = default;

PlatformViewsRegistry::~PlatformViewsRegistry() {
  sInstance.reset();
}

void PlatformViewsRegistry::AddPlatformView(
    const int32_t id,
    std::unique_ptr<PlatformView> platform_view) {
  const std::lock_guard lock(gPlatformViewsRegistryMutex);
  registry_[id] = std::move(platform_view);
}

PlatformView* PlatformViewsRegistry::GetPlatformView(const int32_t id) {
  const std::lock_guard lock(gPlatformViewsRegistryMutex);
  if (registry_.find(id) == registry_.end()) {
    return nullptr;
  }
  return registry_[id].get();
}

void PlatformViewsRegistry::RemovePlatformView(const int32_t id, const bool hybrid) {
  const std::lock_guard lock(gPlatformViewsRegistryMutex);
  const auto it = registry_.find(id);
  if (it != registry_.end()) {
    it->second->Dispose(hybrid);
    it->second.reset();
    registry_.erase(it);
  }
}
