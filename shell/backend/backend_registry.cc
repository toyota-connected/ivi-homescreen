/*
 * Copyright 2026 Toyota Connected North America
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

#include "backend/backend_registry.h"

#include <cassert>
#include <utility>

namespace backend {

BackendRegistry& BackendRegistry::Instance() {
  static BackendRegistry instance;
  return instance;
}

void BackendRegistry::Register(BackendDescriptor d) {
  entries_.push_back(std::move(d));
}

const BackendDescriptor* BackendRegistry::Resolve(std::string_view key) const {
  for (const auto& entry : entries_) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<std::string> BackendRegistry::Keys() const {
  std::vector<std::string> keys;
  keys.reserve(entries_.size());
  for (const auto& entry : entries_) {
    keys.push_back(entry.key);
  }
  return keys;
}

void BackendRegistry::SetActive(const BackendDescriptor* d) {
  active_ = d;
}

const BackendDescriptor& BackendRegistry::Active() const {
  assert(active_ != nullptr);
  return *active_;
}

}  // namespace backend
