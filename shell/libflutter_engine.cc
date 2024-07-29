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

#include "libflutter_engine.h"

#include <iostream>

#include <dlfcn.h>

#include "shared_library.h"

LibFlutterEngineExports::LibFlutterEngineExports(void* lib) {
  if (lib != nullptr) {
    GetFuncAddress(lib, "FlutterEngineCreateAOTData", &CreateAOTData);
    GetFuncAddress(lib, "FlutterEngineCollectAOTData", &CollectAOTData);
    GetFuncAddress(lib, "FlutterEngineRun", &Run);
    GetFuncAddress(lib, "FlutterEngineShutdown", &Shutdown);
    GetFuncAddress(lib, "FlutterEngineInitialize", &Initialize);
    GetFuncAddress(lib, "FlutterEngineDeinitialize", &Deinitialize);
    GetFuncAddress(lib, "FlutterEngineRunInitialized", &RunInitialized);
    GetFuncAddress(lib, "FlutterEngineSendWindowMetricsEvent",
                   &SendWindowMetricsEvent);
    GetFuncAddress(lib, "FlutterEngineSendPointerEvent", &SendPointerEvent);
    GetFuncAddress(lib, "FlutterEngineSendKeyEvent", &SendKeyEvent);
    GetFuncAddress(lib, "FlutterEngineSendPlatformMessage",
                   &SendPlatformMessage);
    GetFuncAddress(lib, "FlutterPlatformMessageCreateResponseHandle",
                   &PlatformMessageCreateResponseHandle);
    GetFuncAddress(lib, "FlutterPlatformMessageReleaseResponseHandle",
                   &PlatformMessageReleaseResponseHandle);
    GetFuncAddress(lib, "FlutterEngineSendPlatformMessageResponse",
                   &SendPlatformMessageResponse);
    GetFuncAddress(lib, "FlutterEngineRegisterExternalTexture",
                   &RegisterExternalTexture);
    GetFuncAddress(lib, "FlutterEngineUnregisterExternalTexture",
                   &UnregisterExternalTexture);
    GetFuncAddress(lib, "FlutterEngineMarkExternalTextureFrameAvailable",
                   &MarkExternalTextureFrameAvailable);
    GetFuncAddress(lib, "FlutterEngineUpdateSemanticsEnabled",
                   &UpdateSemanticsEnabled);
    GetFuncAddress(lib, "FlutterEngineUpdateAccessibilityFeatures",
                   &UpdateAccessibilityFeatures);
    GetFuncAddress(lib, "FlutterEngineDispatchSemanticsAction",
                   &DispatchSemanticsAction);
    GetFuncAddress(lib, "FlutterEngineOnVsync", &OnVsync);
    GetFuncAddress(lib, "FlutterEngineReloadSystemFonts", &ReloadSystemFonts);
    GetFuncAddress(lib, "FlutterEngineTraceEventDurationBegin",
                   &TraceEventDurationBegin);
    GetFuncAddress(lib, "FlutterEngineTraceEventDurationEnd",
                   &TraceEventDurationEnd);
    GetFuncAddress(lib, "FlutterEngineTraceEventInstant", &TraceEventInstant);
    GetFuncAddress(lib, "FlutterEnginePostRenderThreadTask",
                   &PostRenderThreadTask);
    GetFuncAddress(lib, "FlutterEngineGetCurrentTime", &GetCurrentTime);
    GetFuncAddress(lib, "FlutterEngineRunTask", &RunTask);
    GetFuncAddress(lib, "FlutterEngineUpdateLocales", &UpdateLocales);
    GetFuncAddress(lib, "FlutterEngineRunsAOTCompiledDartCode",
                   &RunsAOTCompiledDartCode);
    GetFuncAddress(lib, "FlutterEnginePostDartObject", &PostDartObject);
    GetFuncAddress(lib, "FlutterEngineNotifyLowMemoryWarning",
                   &NotifyLowMemoryWarning);
    GetFuncAddress(lib, "FlutterEnginePostCallbackOnAllNativeThreads",
                   &PostCallbackOnAllNativeThreads);
    GetFuncAddress(lib, "FlutterEngineNotifyDisplayUpdate",
                   &NotifyDisplayUpdate);
    GetFuncAddress(lib, "FlutterEngineScheduleFrame", &ScheduleFrame);
    GetFuncAddress(lib, "FlutterEngineSetNextFrameCallback",
                   &SetNextFrameCallback);
  }
}

LibFlutterEngineExports* LibFlutterEngine::operator->() const {
  return loadExports(nullptr);
}

LibFlutterEngineExports* LibFlutterEngine::loadExports(
    const char* library_path = nullptr) {
  static LibFlutterEngineExports exports = [&] {
    void* lib;

    if (GetProcAddress(RTLD_DEFAULT,
                       "Initialize"))  // Search the global scope
                                       // for pre-loaded library.
    {
      lib = RTLD_DEFAULT;
    } else {
      lib = dlopen(library_path ? library_path : "libflutter_engine.so",
                   RTLD_LAZY | RTLD_LOCAL);
    }

    return LibFlutterEngineExports(lib);
  }();

  return exports.Initialize ? &exports : nullptr;
}

class LibFlutterEngine LibFlutterEngine;
