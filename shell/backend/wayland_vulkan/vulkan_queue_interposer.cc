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

#include "vulkan_queue_interposer.h"

#include <atomic>
#include <cstring>
#include <unordered_map>

namespace wayland_vulkan {

namespace {

// Queue -> guarding mutex. Mutated only at device create/destroy; read on
// every interposed queue call. The map's own lock is uncontended in
// steady state (registration is done before the engine runs) and, when it
// is contended, the callers were about to serialize on the queue mutex
// anyway.
std::mutex g_registry_mutex;
std::unordered_map<VkQueue, std::mutex*>& Registry() {
  static auto* registry = new std::unordered_map<VkQueue, std::mutex*>();
  return *registry;
}

std::mutex* LookupQueueMutex(VkQueue queue) {
  const std::lock_guard<std::mutex> lock(g_registry_mutex);
  const auto& registry = Registry();
  const auto it = registry.find(queue);
  return it != registry.end() ? it->second : nullptr;
}

// RAII guard that locks the registered mutex for a queue, or nothing if the
// queue is unregistered.
class QueueLock {
 public:
  explicit QueueLock(VkQueue queue) : mutex_(LookupQueueMutex(queue)) {
    if (mutex_ != nullptr) {
      mutex_->lock();
    }
  }
  ~QueueLock() {
    if (mutex_ != nullptr) {
      mutex_->unlock();
    }
  }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;

 private:
  std::mutex* mutex_;
};

// Cached real driver procs. Written while the engine resolves its dispatch
// tables (FlutterEngineInitialize / first frame), read on every interposed
// call thereafter. Loader trampolines returned by GIPA/GDPA are valid for
// any dispatchable child object, so a single process-wide slot per name is
// sufficient even with multiple backend instances.
struct RealProcs {
  std::atomic<PFN_vkQueueSubmit> QueueSubmit{nullptr};
  std::atomic<PFN_vkQueueSubmit2> QueueSubmit2{nullptr};
  std::atomic<PFN_vkQueueSubmit2KHR> QueueSubmit2KHR{nullptr};
  std::atomic<PFN_vkQueueWaitIdle> QueueWaitIdle{nullptr};
  std::atomic<PFN_vkQueuePresentKHR> QueuePresentKHR{nullptr};
  std::atomic<PFN_vkQueueBindSparse> QueueBindSparse{nullptr};
  std::atomic<PFN_vkQueueBeginDebugUtilsLabelEXT> QueueBeginDebugUtilsLabelEXT{
      nullptr};
  std::atomic<PFN_vkQueueEndDebugUtilsLabelEXT> QueueEndDebugUtilsLabelEXT{
      nullptr};
  std::atomic<PFN_vkQueueInsertDebugUtilsLabelEXT>
      QueueInsertDebugUtilsLabelEXT{nullptr};
  std::atomic<PFN_vkGetDeviceProcAddr> GetDeviceProcAddr{nullptr};
};

RealProcs g_real;

//------------------------------------------------------------------------
// Locking trampolines. Exact Vulkan signatures; each takes the mutex
// registered for the queue for the duration of the real call.
//------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL
InterposedQueueSubmit(VkQueue queue,
                      uint32_t submitCount,
                      const VkSubmitInfo* pSubmits,
                      VkFence fence) {
  const QueueLock lock(queue);
  return g_real.QueueSubmit.load(std::memory_order_relaxed)(queue, submitCount,
                                                            pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
InterposedQueueSubmit2(VkQueue queue,
                       uint32_t submitCount,
                       const VkSubmitInfo2* pSubmits,
                       VkFence fence) {
  const QueueLock lock(queue);
  return g_real.QueueSubmit2.load(std::memory_order_relaxed)(queue, submitCount,
                                                             pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
InterposedQueueSubmit2KHR(VkQueue queue,
                          uint32_t submitCount,
                          const VkSubmitInfo2* pSubmits,
                          VkFence fence) {
  const QueueLock lock(queue);
  return g_real.QueueSubmit2KHR.load(std::memory_order_relaxed)(
      queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL InterposedQueueWaitIdle(VkQueue queue) {
  const QueueLock lock(queue);
  return g_real.QueueWaitIdle.load(std::memory_order_relaxed)(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL
InterposedQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  const QueueLock lock(queue);
  return g_real.QueuePresentKHR.load(std::memory_order_relaxed)(queue,
                                                                pPresentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL
InterposedQueueBindSparse(VkQueue queue,
                          uint32_t bindInfoCount,
                          const VkBindSparseInfo* pBindInfo,
                          VkFence fence) {
  const QueueLock lock(queue);
  return g_real.QueueBindSparse.load(std::memory_order_relaxed)(
      queue, bindInfoCount, pBindInfo, fence);
}

VKAPI_ATTR void VKAPI_CALL
InterposedQueueBeginDebugUtilsLabelEXT(VkQueue queue,
                                       const VkDebugUtilsLabelEXT* pLabelInfo) {
  const QueueLock lock(queue);
  g_real.QueueBeginDebugUtilsLabelEXT.load(std::memory_order_relaxed)(
      queue, pLabelInfo);
}

VKAPI_ATTR void VKAPI_CALL InterposedQueueEndDebugUtilsLabelEXT(VkQueue queue) {
  const QueueLock lock(queue);
  g_real.QueueEndDebugUtilsLabelEXT.load(std::memory_order_relaxed)(queue);
}

VKAPI_ATTR void VKAPI_CALL InterposedQueueInsertDebugUtilsLabelEXT(
    VkQueue queue,
    const VkDebugUtilsLabelEXT* pLabelInfo) {
  const QueueLock lock(queue);
  g_real.QueueInsertDebugUtilsLabelEXT.load(std::memory_order_relaxed)(
      queue, pLabelInfo);
}

//------------------------------------------------------------------------
// Name -> trampoline dispatch, shared by the instance-level hook and the
// vkGetDeviceProcAddr shim. `resolve` produces the real proc for `name`
// from whichever resolver the engine was about to use, so the trampoline
// always chains to what the engine would otherwise have called.
//------------------------------------------------------------------------

template <typename Resolver>
PFN_vkVoidFunction InterposeQueueProc(const char* name,
                                      const Resolver& resolve) {
  const auto real = resolve(name);
  if (real == nullptr) {
    // Extension not available from this resolver; report that faithfully
    // rather than handing the engine a trampoline over a null pointer.
    return nullptr;
  }
  if (std::strcmp(name, "vkQueueSubmit") == 0) {
    g_real.QueueSubmit.store(reinterpret_cast<PFN_vkQueueSubmit>(real),
                             std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedQueueSubmit);
  }
  if (std::strcmp(name, "vkQueueSubmit2") == 0) {
    g_real.QueueSubmit2.store(reinterpret_cast<PFN_vkQueueSubmit2>(real),
                              std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedQueueSubmit2);
  }
  if (std::strcmp(name, "vkQueueSubmit2KHR") == 0) {
    g_real.QueueSubmit2KHR.store(reinterpret_cast<PFN_vkQueueSubmit2KHR>(real),
                                 std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedQueueSubmit2KHR);
  }
  if (std::strcmp(name, "vkQueueWaitIdle") == 0) {
    g_real.QueueWaitIdle.store(reinterpret_cast<PFN_vkQueueWaitIdle>(real),
                               std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedQueueWaitIdle);
  }
  if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
    g_real.QueuePresentKHR.store(reinterpret_cast<PFN_vkQueuePresentKHR>(real),
                                 std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedQueuePresentKHR);
  }
  if (std::strcmp(name, "vkQueueBindSparse") == 0) {
    g_real.QueueBindSparse.store(reinterpret_cast<PFN_vkQueueBindSparse>(real),
                                 std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedQueueBindSparse);
  }
  if (std::strcmp(name, "vkQueueBeginDebugUtilsLabelEXT") == 0) {
    g_real.QueueBeginDebugUtilsLabelEXT.store(
        reinterpret_cast<PFN_vkQueueBeginDebugUtilsLabelEXT>(real),
        std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(
        InterposedQueueBeginDebugUtilsLabelEXT);
  }
  if (std::strcmp(name, "vkQueueEndDebugUtilsLabelEXT") == 0) {
    g_real.QueueEndDebugUtilsLabelEXT.store(
        reinterpret_cast<PFN_vkQueueEndDebugUtilsLabelEXT>(real),
        std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(
        InterposedQueueEndDebugUtilsLabelEXT);
  }
  if (std::strcmp(name, "vkQueueInsertDebugUtilsLabelEXT") == 0) {
    g_real.QueueInsertDebugUtilsLabelEXT.store(
        reinterpret_cast<PFN_vkQueueInsertDebugUtilsLabelEXT>(real),
        std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(
        InterposedQueueInsertDebugUtilsLabelEXT);
  }
  return nullptr;
}

bool IsQueueProcName(const char* name) {
  return std::strncmp(name, "vkQueue", 7) == 0;
}

// Shim handed to the engine in place of vkGetDeviceProcAddr. The Skia
// embedder path (vulkan::VulkanProcTable) resolves all device-level procs
// through vkGetDeviceProcAddr, which it in turn obtained from the
// instance-level callback — so intercepting only the instance callback
// would leave that path unsynchronized.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
InterposedGetDeviceProcAddr(VkDevice device, const char* pName) {
  const auto real_gdpa =
      g_real.GetDeviceProcAddr.load(std::memory_order_relaxed);
  if (pName != nullptr && IsQueueProcName(pName)) {
    if (auto* proc = InterposeQueueProc(
            pName, [&](const char* n) { return real_gdpa(device, n); })) {
      return proc;
    }
  }
  return real_gdpa(device, pName);
}

}  // namespace

void QueueInterposer::RegisterQueue(VkQueue queue, std::mutex* mutex) {
  const std::lock_guard<std::mutex> lock(g_registry_mutex);
  Registry()[queue] = mutex;
}

void QueueInterposer::UnregisterQueue(VkQueue queue) {
  const std::lock_guard<std::mutex> lock(g_registry_mutex);
  Registry().erase(queue);
}

PFN_vkVoidFunction QueueInterposer::Interpose(VkInstance instance,
                                              const char* procname,
                                              PFN_vkGetInstanceProcAddr gipa) {
  if (procname == nullptr || gipa == nullptr) {
    return nullptr;
  }
  // Impeller initializes its vulkan.hpp dispatcher from the instance-level
  // callback and resolves device functions through it as loader
  // trampolines, so queue entry points arrive here directly.
  if (IsQueueProcName(procname)) {
    return InterposeQueueProc(procname,
                              [&](const char* n) { return gipa(instance, n); });
  }
  // The Skia VulkanProcTable path resolves device procs via
  // vkGetDeviceProcAddr; hand it a shim so those resolutions are also
  // interposed.
  if (std::strcmp(procname, "vkGetDeviceProcAddr") == 0) {
    const auto real = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gipa(instance, "vkGetDeviceProcAddr"));
    if (real == nullptr) {
      return nullptr;
    }
    g_real.GetDeviceProcAddr.store(real, std::memory_order_relaxed);
    return reinterpret_cast<PFN_vkVoidFunction>(InterposedGetDeviceProcAddr);
  }
  return nullptr;
}

}  // namespace wayland_vulkan
