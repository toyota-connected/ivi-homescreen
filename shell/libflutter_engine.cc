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
    ShellGetFuncAddress(lib, "FlutterEngineCreateAOTData", &CreateAOTData);
    ShellGetFuncAddress(lib, "FlutterEngineCollectAOTData", &CollectAOTData);
    ShellGetFuncAddress(lib, "FlutterEngineRun", &Run);
    ShellGetFuncAddress(lib, "FlutterEngineShutdown", &Shutdown);
    ShellGetFuncAddress(lib, "FlutterEngineInitialize", &Initialize);
    ShellGetFuncAddress(lib, "FlutterEngineDeinitialize", &Deinitialize);
    ShellGetFuncAddress(lib, "FlutterEngineRunInitialized", &RunInitialized);
    ShellGetFuncAddress(lib, "FlutterEngineSendWindowMetricsEvent",
                        &SendWindowMetricsEvent);
    ShellGetFuncAddress(lib, "FlutterEngineSendPointerEvent",
                        &SendPointerEvent);
    ShellGetFuncAddress(lib, "FlutterEngineSendKeyEvent", &SendKeyEvent);
    ShellGetFuncAddress(lib, "FlutterEngineSendPlatformMessage",
                        &SendPlatformMessage);
    ShellGetFuncAddress(lib, "FlutterPlatformMessageCreateResponseHandle",
                        &PlatformMessageCreateResponseHandle);
    ShellGetFuncAddress(lib, "FlutterPlatformMessageReleaseResponseHandle",
                        &PlatformMessageReleaseResponseHandle);
    ShellGetFuncAddress(lib, "FlutterEngineSendPlatformMessageResponse",
                        &SendPlatformMessageResponse);
    ShellGetFuncAddress(lib, "FlutterEngineRegisterExternalTexture",
                        &RegisterExternalTexture);
    ShellGetFuncAddress(lib, "FlutterEngineUnregisterExternalTexture",
                        &UnregisterExternalTexture);
    ShellGetFuncAddress(lib, "FlutterEngineMarkExternalTextureFrameAvailable",
                        &MarkExternalTextureFrameAvailable);
    ShellGetFuncAddress(lib, "FlutterEngineUpdateSemanticsEnabled",
                        &UpdateSemanticsEnabled);
    ShellGetFuncAddress(lib, "FlutterEngineUpdateAccessibilityFeatures",
                        &UpdateAccessibilityFeatures);
    ShellGetFuncAddress(lib, "FlutterEngineDispatchSemanticsAction",
                        &DispatchSemanticsAction);
    ShellGetFuncAddress(lib, "FlutterEngineOnVsync", &OnVsync);
    ShellGetFuncAddress(lib, "FlutterEngineReloadSystemFonts",
                        &ReloadSystemFonts);
    ShellGetFuncAddress(lib, "FlutterEngineTraceEventDurationBegin",
                        &TraceEventDurationBegin);
    ShellGetFuncAddress(lib, "FlutterEngineTraceEventDurationEnd",
                        &TraceEventDurationEnd);
    ShellGetFuncAddress(lib, "FlutterEngineTraceEventInstant",
                        &TraceEventInstant);
    ShellGetFuncAddress(lib, "FlutterEnginePostRenderThreadTask",
                        &PostRenderThreadTask);
    ShellGetFuncAddress(lib, "FlutterEngineGetCurrentTime", &GetCurrentTime);
    ShellGetFuncAddress(lib, "FlutterEngineRunTask", &RunTask);
    ShellGetFuncAddress(lib, "FlutterEngineUpdateLocales", &UpdateLocales);
    ShellGetFuncAddress(lib, "FlutterEngineRunsAOTCompiledDartCode",
                        &RunsAOTCompiledDartCode);
    ShellGetFuncAddress(lib, "FlutterEnginePostDartObject", &PostDartObject);
    ShellGetFuncAddress(lib, "FlutterEngineNotifyLowMemoryWarning",
                        &NotifyLowMemoryWarning);
    ShellGetFuncAddress(lib, "FlutterEnginePostCallbackOnAllNativeThreads",
                        &PostCallbackOnAllNativeThreads);
    ShellGetFuncAddress(lib, "FlutterEngineNotifyDisplayUpdate",
                        &NotifyDisplayUpdate);
    ShellGetFuncAddress(lib, "FlutterEngineScheduleFrame", &ScheduleFrame);
    ShellGetFuncAddress(lib, "FlutterEngineSetNextFrameCallback",
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

    if (ShellGetProcAddress(RTLD_DEFAULT,
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
