#pragma once

#include "flutter_desktop_view.h"
#include "keyboard_hook_handler.h"

struct FlutterDesktopEngineState;
struct FlutterDesktopView;

class Engine;
class FlutterView;
namespace flutter {
class KeyboardHookHandler;
}

// Struct for storing state within an instance of the View.
struct FlutterDesktopViewControllerState {
  // Flutter View reference
  FlutterView* view{};

  // Engine Class reference
  Engine* engine{};

  // The state associated with the engine.
  std::unique_ptr<FlutterDesktopEngineState> engine_state{};

  // The window handle given to API clients.
  std::unique_ptr<FlutterDesktopView> view_wrapper{};

  // Handlers for keyboard events from Display.
  std::vector<std::unique_ptr<flutter::KeyboardHookHandler>>
      keyboard_hook_handlers;
};
