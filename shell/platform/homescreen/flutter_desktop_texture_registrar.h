#pragma once

struct FlutterDesktopEngineState;

// State associated with the texture registrar.
struct FlutterDesktopTextureRegistrar {
  // The engine that backs this registrar.
  FlutterDesktopEngineState* engine;
};
