#pragma once

#include <asio/io_context_strand.hpp>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/plugin_registrar.h"
#include "flutter/shell/platform/common/incoming_message_dispatcher.h"
#include "flutter_desktop_plugin_registrar.h"
#include "flutter_desktop_texture_registrar.h"
#include "flutter_desktop_view_controller_state.h"
#include "platform/homescreen/logging_handler.h"
#include "platform/homescreen/mouse_cursor_handler.h"
#include "platform/homescreen/platform_handler.h"
#include "platform/homescreen/platform_views/platform_views_handler.h"
#include "shell/libflutter_engine.h"
#include "shell/task_runner.h"

struct FlutterDesktopViewControllerState;
struct FlutterDesktopMessenger;

class Backend;
class Engine;
class PlatformHandler;
class PlatformViewsHandler;

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

  std::unique_ptr<PlatformViewsHandler> platform_views_handler{};

  std::unique_ptr<MouseCursorHandler> mouse_cursor_handler{};

  std::unique_ptr<LoggingHandler> logging_handler{};

  AccessibilityTree* accessibility_tree = nullptr;

  // The controller associated with this engine instance, if any.
  // This will always be null for a headless engine.
  FlutterDesktopViewControllerState* view_controller = nullptr;

  // Cached pointer to the backend that owns the render context for this
  // engine. Wired by FlutterView::Initialize() before Engine::Run and
  // used by the FlutterRendererConfig lambdas (make_current,
  // clear_current, present, etc.) to skip the
  // view_controller->engine->GetBackend() chain.
  //
  // This is load-bearing for SIGTERM shutdown: FlutterView's m_state
  // (which owns view_controller) destructs BEFORE m_flutter_engine in
  // member-order, so Engine::~Engine's call into FlutterEngineDeinitialize
  // fires render callbacks against a freed view_controller. Reading
  // backend directly off engine_state avoids the dangling indirection
  // — engine_state is owned by Engine (lives through Deinitialize) and
  // the Backend itself is m_backend in FlutterView (declared first,
  // destructs last), so the pointer stays valid until after Engine's
  // teardown completes.
  Backend* backend = nullptr;

  // AOT data for this engine instance, if applicable.
  UniqueAotDataPtr aot_data = nullptr;

  // Flutter Asset Folder
  std::string flutter_asset_directory;
};
