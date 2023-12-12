#pragma once

#include <engine.h>

#include "flutter_desktop_engine_state.h"
#include "flutter_desktop_view.h"

struct FlutterDesktopEngineState;
struct FlutterDesktopView;

class Engine;
class FlutterView;

// Struct for storing state within an instance of the View.
struct FlutterDesktopViewControllerState {
  // Flutter View reference
  FlutterView* view{};

  // Engine Class reference
  Engine *engine{};

  // The state associated with the engine.
  std::unique_ptr<FlutterDesktopEngineState> engine_state{};

  // The window handle given to API clients.
  std::unique_ptr<FlutterDesktopView> view_wrapper{};

  // Handlers for keyboard events from Dislay.
  // std::vector<std::unique_ptr<flutter::KeyboardHookHandler>>
  // keyboard_hook_handlers;
};
