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

  // The state associated with the engine. Non-owning — `Engine` owns the
  // unique_ptr so that engine_state outlives FlutterEngineDeinitialize.
  // The previous arrangement (unique_ptr here) freed engine_state when
  // FlutterView::m_state destructed, but Engine::~Engine then called
  // FlutterEngineDeinitialize which posts platform-message tasks whose
  // user_data is this engine_state pointer — UAF in OnFlutterPlatformMessage
  // → IncomingMessageDispatcher::HandleMessage(this=nullptr).
  FlutterDesktopEngineState* engine_state{};

  // The window handle given to API clients.
  std::unique_ptr<FlutterDesktopView> view_wrapper{};

  // Handlers for keyboard events from Display.
  std::vector<std::unique_ptr<flutter::KeyboardHookHandler>>
      keyboard_hook_handlers;
};
