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

#include "backend/headless_vulkan/vk_export_api.h"

#include <atomic>
#include <cstring>

#include "backend/headless_vulkan/headless_vulkan.h"
#include "ihs/vk_export.h"

namespace {

// The single active backend the exported ABI delegates to. Set by the backend
// constructor, cleared by its destructor. Atomic because the bridge calls the
// ABI from its own IO thread while the shell threads construct/destroy it.
std::atomic<HeadlessVulkanBackend*> g_backend{nullptr};

// ── C ABI entry points ───────────────────────────────────────────────────────
// Each loads g_backend and delegates, returning a safe default when no
// headless-vulkan backend is active.

IhsVkExportCaps ApiGetCaps() {
  IhsVkExportCaps caps{};
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    b->FillExportCaps(&caps);
  } else {
    caps.api_version = IHS_VK_EXPORT_API_VERSION;
    caps.export_active = 0;
  }
  return caps;
}

int ApiGetImageTable(IhsVkExportImageTable* out) {
  if (out == nullptr) {
    return -1;
  }
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    return b->FillImageTable(out);
  }
  std::memset(out, 0, sizeof(*out));
  return -1;
}

void ApiSetFrameListener(IhsVkFrameListener cb, void* user) {
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    b->SetExportFrameListener(cb, user);
  }
}

void ApiReleaseFrame(uint32_t slot) {
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    b->OnConsumerReleaseFrame(slot);
  }
}

void ApiSetPaceSource(uint32_t src) {
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    b->SetExportPaceSource(src);
  }
}

void ApiInjectPointer(const IhsPointerEvent* ev) {
  if (ev == nullptr) {
    return;
  }
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    b->InjectPointer(*ev);
  }
}

int ApiRebuildPool(uint32_t width, uint32_t height) {
  if (auto* b = g_backend.load(std::memory_order_acquire)) {
    return b->RebuildExportPool(width, height);
  }
  return -1;
}

const IhsVkExportApiV1 kApi = {
    ApiGetCaps,       ApiGetImageTable, ApiSetFrameListener, ApiReleaseFrame,
    ApiSetPaceSource, ApiInjectPointer, ApiRebuildPool,
};

}  // namespace

namespace ihs {

void RegisterVkExportBackend(HeadlessVulkanBackend* b) {
  g_backend.store(b, std::memory_order_release);
}

void UnregisterVkExportBackend(HeadlessVulkanBackend* b) {
  // Only clear the slot if it still points at this backend; a stale destructor
  // must not clobber a newer registration.
  HeadlessVulkanBackend* expected = b;
  g_backend.compare_exchange_strong(expected, nullptr,
                                    std::memory_order_acq_rel);
}

}  // namespace ihs

// The one exported shell symbol. Default visibility + `used` so it survives
// into the executable's dynamic symbol table (the CMake target also passes
// --export-dynamic-symbol=ihs_vk_export_get_api). Returns NULL when no
// headless-vulkan backend is active or the requested version is unsupported.
extern "C" __attribute__((visibility("default"), used)) const IhsVkExportApiV1*
ihs_vk_export_get_api(uint32_t requested_version) {
  if (requested_version != IHS_VK_EXPORT_API_VERSION) {
    return nullptr;
  }
  return g_backend.load(std::memory_order_acquire) ? &kApi : nullptr;
}
