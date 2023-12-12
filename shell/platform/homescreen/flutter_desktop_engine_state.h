#pragma once

#include "libflutter_engine.h"

#include "flutter/shell/platform/common/client_wrapper/include/flutter/plugin_registrar.h"
#include "flutter/shell/platform/common/incoming_message_dispatcher.h"
#include "flutter_desktop_view_controller_state.h"
#include "flutter_desktop_plugin_registrar.h"
#include "platform/homescreen/platform_handler.h"
#include "plugins/isolate/isolate.h"
#include "engine.h"

struct FlutterDesktopViewControllerState;
struct FlutterDesktopMessenger;

class Engine;
class IsolateHandler;
class PlatformHandler;

// Custom deleter for FlutterEngineAOTData.
struct AOTDataDeleter {
  void operator()(const FlutterEngineAOTData aot_data) const {
    LibFlutterEngine->CollectAOTData(aot_data);
  }
};

using FlutterDesktopMessengerReferenceOwner =
    std::unique_ptr<FlutterDesktopMessenger,
                    decltype(&FlutterDesktopMessengerRelease)>;

using UniqueAotDataPtr = std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter>;
/// Maintains one ref on the FlutterDesktopMessenger's internal reference count.

// Struct for storing state of a Flutter engine instance.
struct FlutterDesktopEngineState {
  // The handle to the Flutter engine instance.
  FLUTTER_API_SYMBOL(FlutterEngine) flutter_engine{};

  // The event loop for the main thread that allows for delayed task execution.
  // std::unique_ptr<flutter::EventLoop> event_loop;

  // The plugin messenger handle given to API clients.
  FlutterDesktopMessengerReferenceOwner messenger = {
    nullptr, [](FlutterDesktopMessengerRef /* ref */) {}};

  // Message dispatch manager for messages from the Flutter engine.
  std::unique_ptr<flutter::IncomingMessageDispatcher> message_dispatcher;

  // The plugin registrar handle given to API clients.
  std::unique_ptr<FlutterDesktopPluginRegistrar> plugin_registrar;

  // The plugin registrar managing internal plugins.
  std::unique_ptr<flutter::PluginRegistrar> internal_plugin_registrar;

  // Handler for the flutter/platform channel.
  std::unique_ptr<PlatformHandler> platform_handler{};

  // Handler for the flutter/isolate channel.
  std::unique_ptr<IsolateHandler> isolate_handler{};

  // The controller associated with this engine instance, if any.
  // This will always be null for a headless engine.
  FlutterDesktopViewControllerState* view_controller = nullptr;

  // AOT data for this engine instance, if applicable.
  UniqueAotDataPtr aot_data = nullptr;
};
