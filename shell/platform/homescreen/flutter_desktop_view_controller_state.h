#pragma once

#include <memory>

#include "flutter_desktop_view.h"
#include "keyboard_hook_handler.h"

struct FlutterDesktopEngineState;
struct FlutterDesktopView;

class Engine;
class FlutterView;
namespace flutter {
class KeyboardHookHandler;
class TextInputPlugin;
}  // namespace flutter

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

  // TextInputPlugin is owned here so that KeyEventHandler can hold a raw
  // pointer to it for fallback text insertion after embedder key dispatch.
  // IMPORTANT: declared *before* keyboard_hook_handlers so that C++'s
  // reverse-declaration destruction order tears down keyboard_hook_handlers
  // (which contains KeyEventHandler holding a raw pointer to this plugin)
  // before text_input_plugin itself is freed.  See H1 in the reliability
  // notes.
  std::unique_ptr<flutter::TextInputPlugin> text_input_plugin;

  // Handlers for keyboard events from Display.
  std::vector<std::unique_ptr<flutter::KeyboardHookHandler>>
      keyboard_hook_handlers;
};
