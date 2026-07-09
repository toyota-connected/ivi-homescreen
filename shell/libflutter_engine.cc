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

#include <dlfcn.h>

#include <cstdlib>
#include <mutex>
#include <string>

#include "ihs/trace.h"
#include "logging.h"
#include "shared_library.h"

namespace {

constexpr char kDefaultSoName[] = "libflutter_engine.so";
constexpr char kPreloadedSentinel[] = "<preloaded>";

// Backing storage for the export table. Written only inside the
// call_once body; published to readers via LibFlutterEngine::table_.
LibFlutterEngineExports g_exports;
std::once_flag g_load_once;
std::string g_loaded_path;

/**
 * Resolve every export from `lib`, which is either a dlopen() handle or
 * the RTLD_DEFAULT pseudo-handle. RTLD_DEFAULT is ((void*)0) on glibc,
 * so it must never be null-tested as if it were a real handle — dlsym()
 * accepts it directly.
 *
 * Returns false if any *required* export is missing. AddView/RemoveView
 * are optional (version-gated) ABI and remain null on older engines.
 */
bool PopulateExports(void* lib) {
  bool ok = true;

  const auto require = [lib, &ok](auto* out, const char* name) {
    ShellGetFuncAddress(lib, name, out);
    if (*out == nullptr) {
      spdlog::critical("libflutter_engine: required export missing: {}", name);
      ok = false;
    }
  };
  const auto optional = [lib](auto* out, const char* name) {
    ShellGetFuncAddress(lib, name, out);
  };

  require(&g_exports.CreateAOTData, "FlutterEngineCreateAOTData");
  require(&g_exports.CollectAOTData, "FlutterEngineCollectAOTData");
  require(&g_exports.Run, "FlutterEngineRun");
  require(&g_exports.Shutdown, "FlutterEngineShutdown");
  require(&g_exports.Initialize, "FlutterEngineInitialize");
  require(&g_exports.Deinitialize, "FlutterEngineDeinitialize");
  require(&g_exports.RunInitialized, "FlutterEngineRunInitialized");
  require(&g_exports.SendWindowMetricsEvent,
          "FlutterEngineSendWindowMetricsEvent");
  require(&g_exports.SendPointerEvent, "FlutterEngineSendPointerEvent");
  require(&g_exports.SendKeyEvent, "FlutterEngineSendKeyEvent");
  require(&g_exports.SendPlatformMessage, "FlutterEngineSendPlatformMessage");
  require(&g_exports.PlatformMessageCreateResponseHandle,
          "FlutterPlatformMessageCreateResponseHandle");
  require(&g_exports.PlatformMessageReleaseResponseHandle,
          "FlutterPlatformMessageReleaseResponseHandle");
  require(&g_exports.SendPlatformMessageResponse,
          "FlutterEngineSendPlatformMessageResponse");
  require(&g_exports.RegisterExternalTexture,
          "FlutterEngineRegisterExternalTexture");
  require(&g_exports.UnregisterExternalTexture,
          "FlutterEngineUnregisterExternalTexture");
  require(&g_exports.MarkExternalTextureFrameAvailable,
          "FlutterEngineMarkExternalTextureFrameAvailable");
  require(&g_exports.UpdateSemanticsEnabled,
          "FlutterEngineUpdateSemanticsEnabled");
  require(&g_exports.UpdateAccessibilityFeatures,
          "FlutterEngineUpdateAccessibilityFeatures");
  require(&g_exports.DispatchSemanticsAction,
          "FlutterEngineDispatchSemanticsAction");
  require(&g_exports.OnVsync, "FlutterEngineOnVsync");
  require(&g_exports.ReloadSystemFonts, "FlutterEngineReloadSystemFonts");
  require(&g_exports.TraceEventDurationBegin,
          "FlutterEngineTraceEventDurationBegin");
  require(&g_exports.TraceEventDurationEnd,
          "FlutterEngineTraceEventDurationEnd");
  require(&g_exports.TraceEventInstant, "FlutterEngineTraceEventInstant");
  require(&g_exports.PostRenderThreadTask, "FlutterEnginePostRenderThreadTask");
  require(&g_exports.GetCurrentTime, "FlutterEngineGetCurrentTime");
  require(&g_exports.RunTask, "FlutterEngineRunTask");
  require(&g_exports.UpdateLocales, "FlutterEngineUpdateLocales");
  require(&g_exports.RunsAOTCompiledDartCode,
          "FlutterEngineRunsAOTCompiledDartCode");
  require(&g_exports.PostDartObject, "FlutterEnginePostDartObject");
  require(&g_exports.NotifyLowMemoryWarning,
          "FlutterEngineNotifyLowMemoryWarning");
  require(&g_exports.PostCallbackOnAllNativeThreads,
          "FlutterEnginePostCallbackOnAllNativeThreads");
  require(&g_exports.NotifyDisplayUpdate, "FlutterEngineNotifyDisplayUpdate");
  require(&g_exports.ScheduleFrame, "FlutterEngineScheduleFrame");
  require(&g_exports.SetNextFrameCallback, "FlutterEngineSetNextFrameCallback");

  optional(&g_exports.AddView, "FlutterEngineAddView");
  optional(&g_exports.RemoveView, "FlutterEngineRemoveView");

  return ok;
}

}  // namespace

std::atomic<const LibFlutterEngineExports*> LibFlutterEngine::table_{nullptr};

bool LibFlutterEngine::Load(const char* library_path) {
  const std::string requested = library_path ? library_path : kDefaultSoName;

  std::call_once(g_load_once, [&requested] {
    // A preloaded engine (statically linked into the executable or
    // LD_PRELOADed) takes precedence over any requested path. Probe the
    // engine's actual export — a generic symbol name here would
    // false-positive on unrelated libraries in global scope.
    void* lib = RTLD_DEFAULT;
    if (ShellGetProcAddress(RTLD_DEFAULT, "FlutterEngineInitialize") !=
        nullptr) {
      g_loaded_path = kPreloadedSentinel;
      spdlog::info("libflutter_engine: using preloaded engine");
    } else {
      dlerror();  // clear stale error state
      lib = dlopen(requested.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (lib == nullptr) {
        const char* reason = dlerror();
        spdlog::critical("libflutter_engine: dlopen({}) failed: {}", requested,
                         reason ? reason : "unknown reason");
        return;  // table_ stays null; failure is cached for process life
      }
      g_loaded_path = requested;
    }

    if (!PopulateExports(lib)) {
      // Required ABI incomplete: engine .so too old, or the wrong
      // artifact. Clear any partially populated table. The dlopen
      // handle is deliberately not dlclose()d.
      g_exports = {};
      return;
    }

    table_.store(&g_exports, std::memory_order_release);

    // Install the engine's trace procs as the ihs_shared trace sink, so shell
    // and plugin trace events route into the Flutter/Perfetto timeline. Events
    // emitted before this point (backend bring-up, modeset) fall back to the
    // ftrace trace_marker sink inside ihs_shared.
    static const IhsTraceEngineProcs kTraceProcs = {
        sizeof(IhsTraceEngineProcs),
        g_exports.TraceEventDurationBegin,
        g_exports.TraceEventDurationEnd,
        g_exports.TraceEventInstant,
    };
    ihs_trace_set_engine_procs(&kTraceProcs);
  });

  if (table_.load(std::memory_order_acquire) == nullptr) {
    return false;
  }

  // The engine is bound once per process. A later request for a
  // *different* path cannot be honored — fail loudly instead of
  // silently returning the wrong library's exports.
  if (g_loaded_path != kPreloadedSentinel && requested != g_loaded_path) {
    spdlog::error(
        "libflutter_engine: already loaded from '{}'; ignoring request "
        "for '{}'",
        g_loaded_path, requested);
    return false;
  }

  return true;
}

void LibFlutterEngine::FailNotLoaded() {
  spdlog::critical(
      "LibFlutterEngine used before a successful Load(); aborting");
  std::abort();
}

class LibFlutterEngine LibFlutterEngine;
