#pragma once

#include <asio/io_context_strand.hpp>
#include <string>

#include "config/common.h"

#include "flutter/shell/platform/common/client_wrapper/include/flutter/plugin_registrar.h"
#include "flutter/shell/platform/common/incoming_message_dispatcher.h"
#include "flutter_desktop_plugin_registrar.h"
#include "flutter_desktop_texture_registrar.h"
#include "flutter_desktop_view_controller_state.h"
#include "platform/homescreen/logging_handler.h"
#include "platform/homescreen/mouse_cursor_handler.h"
#include "platform/homescreen/platform_handler.h"
#if BUILD_WATCHDOG
#include "platform/homescreen/watchdog_plugin.h"
#endif
#if ENABLE_OSGI
#include "osgi/osgi_bridge_plugin.h"
#endif
#include "platform/homescreen/platform_views/platform_view_registry.h"
#include "platform/homescreen/platform_views/platform_views_handler.h"
#include "shell/libflutter_engine.h"
#include "shell/task_runner.h"

struct FlutterDesktopViewControllerState;
struct FlutterDesktopMessenger;

class AccessibilityTree;
class Engine;
class PlatformHandler;
class PlatformViewsHandler;
class PlatformViewRegistry;

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

extern "C" {
// Held only as a pointer; the hub header is not needed here.
struct IhsSemanticsSource;
}

// Struct for storing state of a Flutter engine instance.
struct FlutterDesktopEngineState {
  // The handle to the Flutter engine instance.
  FLUTTER_API_SYMBOL(FlutterEngine) flutter_engine {};

  // The platform channel execution thread.
  TaskRunner* platform_task_runner{};

  // The plugin messenger handle given to API clients.
  FlutterDesktopMessengerReferenceOwner messenger = {
      nullptr, [](FlutterDesktopMessengerRef /* ref */) {}};

  // Message dispatch manager for messages from the Flutter engine.
  std::unique_ptr<flutter::IncomingMessageDispatcher> message_dispatcher;

  // The plugin registrar handle given to API clients.
  std::unique_ptr<FlutterDesktopPluginRegistrar> plugin_registrar;

  // The plugin registrar handle given to API clients.
  std::unique_ptr<FlutterDesktopTextureRegistrar> texture_registrar;

  // The plugin registrar managing internal plugins.
  std::unique_ptr<flutter::PluginRegistrar> internal_plugin_registrar;

  // Handler for the flutter/platform channel.
  std::unique_ptr<PlatformHandler> platform_handler{};

  // Owns platform-view id->instance lifecycle + the view-type factory table.
  // Declared before the handler so it outlives the handler that adapts to it,
  // and is the object the shared-module C API fronts.
  std::unique_ptr<PlatformViewRegistry> platform_view_registry{};

  std::unique_ptr<PlatformViewsHandler> platform_views_handler{};

  std::unique_ptr<MouseCursorHandler> mouse_cursor_handler{};

  std::unique_ptr<LoggingHandler> logging_handler{};

#if BUILD_WATCHDOG
  std::unique_ptr<WatchdogPlugin> watchdog_handler{};
#endif

#if ENABLE_OSGI
  std::unique_ptr<ihs::osgi::OsgiBridgePlugin> osgi_bridge_handler{};
#endif

  AccessibilityTree* accessibility_tree = nullptr;

  // The engine that owns this state. Semantics batches arrive on a static
  // callback that is handed only this struct, and routing one to the view it
  // describes needs the per-view state the engine holds.
  Engine* engine = nullptr;

  // The hub registration this engine publishes its tree to, travelling beside
  // the tree because the two are only meaningful together: publishing without
  // it means the last engine to run owns the only tree there is.
  IhsSemanticsSource* semantics_source = nullptr;

  // The controller associated with this engine instance, if any.
  // This will always be null for a headless engine.
  FlutterDesktopViewControllerState* view_controller = nullptr;

  // AOT data for this engine instance, if applicable.
  UniqueAotDataPtr aot_data = nullptr;

  // Flutter Asset Folder
  std::string flutter_asset_directory;
};
