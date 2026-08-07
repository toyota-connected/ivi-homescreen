/*
 * Copyright 2023 Toyota Connected North America
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

#include <atomic>

#include "flutter/shell/platform/embedder/embedder.h"

struct LibFlutterEngineExports {
  FlutterEngineCreateAOTDataFnPtr CreateAOTData = nullptr;
  FlutterEngineCollectAOTDataFnPtr CollectAOTData = nullptr;
  FlutterEngineRunFnPtr Run = nullptr;
  FlutterEngineShutdownFnPtr Shutdown = nullptr;
  FlutterEngineInitializeFnPtr Initialize = nullptr;
  FlutterEngineDeinitializeFnPtr Deinitialize = nullptr;
  FlutterEngineRunInitializedFnPtr RunInitialized = nullptr;
  FlutterEngineSendWindowMetricsEventFnPtr SendWindowMetricsEvent = nullptr;
  FlutterEngineSendPointerEventFnPtr SendPointerEvent = nullptr;
  FlutterEngineSendKeyEventFnPtr SendKeyEvent = nullptr;
  FlutterEngineSendPlatformMessageFnPtr SendPlatformMessage = nullptr;
  FlutterEnginePlatformMessageCreateResponseHandleFnPtr
      PlatformMessageCreateResponseHandle = nullptr;
  FlutterEnginePlatformMessageReleaseResponseHandleFnPtr
      PlatformMessageReleaseResponseHandle = nullptr;
  FlutterEngineSendPlatformMessageResponseFnPtr SendPlatformMessageResponse =
      nullptr;
  FlutterEngineRegisterExternalTextureFnPtr RegisterExternalTexture = nullptr;
  FlutterEngineUnregisterExternalTextureFnPtr UnregisterExternalTexture =
      nullptr;
  FlutterEngineMarkExternalTextureFrameAvailableFnPtr
      MarkExternalTextureFrameAvailable = nullptr;
  FlutterEngineUpdateSemanticsEnabledFnPtr UpdateSemanticsEnabled = nullptr;
  FlutterEngineUpdateAccessibilityFeaturesFnPtr UpdateAccessibilityFeatures =
      nullptr;
  FlutterEngineDispatchSemanticsActionFnPtr DispatchSemanticsAction = nullptr;
  /* Optional: added to the embedder ABI after the oldest engine this port
   * supports, so it stays null on an older libflutter_engine.so. Callers must
   * null-test and fall back to DispatchSemanticsAction, which loses only the
   * view id. Requiring it would turn an older engine from degraded into one
   * the shell refuses to start against. */
  FlutterEngineSendSemanticsActionFnPtr SendSemanticsAction = nullptr;
  FlutterEngineOnVsyncFnPtr OnVsync = nullptr;
  FlutterEngineReloadSystemFontsFnPtr ReloadSystemFonts = nullptr;
  FlutterEngineTraceEventDurationBeginFnPtr TraceEventDurationBegin = nullptr;
  FlutterEngineTraceEventDurationEndFnPtr TraceEventDurationEnd = nullptr;
  FlutterEngineTraceEventInstantFnPtr TraceEventInstant = nullptr;
  FlutterEnginePostRenderThreadTaskFnPtr PostRenderThreadTask = nullptr;
  FlutterEngineGetCurrentTimeFnPtr GetCurrentTime = nullptr;
  FlutterEngineRunTaskFnPtr RunTask = nullptr;
  FlutterEngineUpdateLocalesFnPtr UpdateLocales = nullptr;
  FlutterEngineRunsAOTCompiledDartCodeFnPtr RunsAOTCompiledDartCode = nullptr;
  FlutterEnginePostDartObjectFnPtr PostDartObject = nullptr;
  FlutterEngineNotifyLowMemoryWarningFnPtr NotifyLowMemoryWarning = nullptr;
  FlutterEnginePostCallbackOnAllNativeThreadsFnPtr
      PostCallbackOnAllNativeThreads = nullptr;
  FlutterEngineNotifyDisplayUpdateFnPtr NotifyDisplayUpdate = nullptr;
  FlutterEngineScheduleFrameFnPtr ScheduleFrame = nullptr;
  FlutterEngineSetNextFrameCallbackFnPtr SetNextFrameCallback = nullptr;
  // Multi-view. Both are async; the add/remove callbacks in
  // FlutterAddViewInfo/FlutterRemoveViewInfo report completion.
  // Optional ABI: may be null on an engine .so too old to export them.
  // Call sites must null-check before use.
  FlutterEngineAddViewFnPtr AddView = nullptr;
  FlutterEngineRemoveViewFnPtr RemoveView = nullptr;
};

class LibFlutterEngine {
 public:
  /**
   * @brief One-shot loader. Call from engine bring-up, before any
   *        operator-> use.
   * @param[in] library_path path to the engine shared library, or nullptr
   *            for the default "libflutter_engine.so". A preloaded engine
   *            (statically linked or LD_PRELOADed, detected via the
   *            FlutterEngineInitialize export in global scope) takes
   *            precedence over any path.
   * @return bool
   * @retval true  engine loaded and required ABI validated
   * @retval false dlopen failed, a required export is missing, or a prior
   *               Load() bound a different library path. The reason is
   *               logged. Failure is cached for the process lifetime;
   *               Load() never retries.
   * @relation
   * internal
   *
   * Thread-safe: the load runs under std::call_once; the export table is
   * published with release semantics after successful validation.
   */
  static bool Load(const char* library_path = nullptr);

  /**
   * @brief Whether a successful Load() has completed.
   * @return bool
   * @relation
   * internal
   */
  static bool IsPresent() {
    return table_.load(std::memory_order_acquire) != nullptr;
  }

  /**
   * @brief Access the validated export table.
   *
   * Aborts with a critical log if used before a successful Load() —
   * a diagnosable failure at the boundary instead of a null-pointer
   * dereference several frames deep.
   */
  const LibFlutterEngineExports* operator->() const {
    const auto* table = table_.load(std::memory_order_acquire);
    if (table == nullptr) {
      FailNotLoaded();
    }
    return table;
  }

 private:
  [[noreturn]] static void FailNotLoaded();

  // Null until Load() succeeds; thereafter points to the immutable,
  // fully populated export table. Acquire/release pairing makes the
  // table contents visible to any thread that observes the pointer.
  static std::atomic<const LibFlutterEngineExports*> table_;
};

extern class LibFlutterEngine LibFlutterEngine;
