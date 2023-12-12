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

#include "flutter/shell/platform/embedder/embedder.h"

struct LibFlutterEngineExports {
  LibFlutterEngineExports() = default;
  explicit LibFlutterEngineExports(void* lib);

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
};

class LibFlutterEngine {
 public:
  static bool IsPresent(const char* library_path = nullptr) {
    return loadExports(library_path) != nullptr;
  }

  LibFlutterEngineExports* operator->() const;

 private:
  static LibFlutterEngineExports* loadExports(const char* library_path);
};

extern LibFlutterEngine LibFlutterEngine;
