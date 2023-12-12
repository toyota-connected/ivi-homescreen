#pragma once

#include "flutter_desktop_engine_state.h"

struct FlutterDesktopEngineState;

// State associated with the plugin registrar.
struct FlutterDesktopPluginRegistrar {
  // The engine that backs this registrar.
  FlutterDesktopEngineState* engine;

  // Callback to be called on registrar destruction.
  FlutterDesktopOnPluginRegistrarDestroyed destruction_handler;
};
