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

#include "backend/drm_kms_egl/drm_backend.h"

#include <utility>

std::unique_ptr<DrmBackend> DrmBackend::Create(const DrmConfig& cfg) {
  return std::unique_ptr<DrmBackend>(new DrmBackend(cfg));
}

DrmBackend::DrmBackend(DrmConfig  cfg) : cfg_(std::move(cfg)) {}

DrmBackend::~DrmBackend() = default;

bool DrmBackend::MakeCurrent() {
  return false;
}

bool DrmBackend::ClearCurrent() {
  return false;
}

bool DrmBackend::Present() {
  return false;
}

uint32_t DrmBackend::FboCallback(const FlutterFrameInfo* /*info*/) {
  return 0;
}

void* DrmBackend::ProcResolver(const char* /*name*/) {
  return nullptr;
}

FlutterCompositor DrmBackend::MakeCompositor() {
  return FlutterCompositor{};
}
