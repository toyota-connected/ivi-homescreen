/*
 * Copyright 2026 Toyota Connected North America
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

#pragma once

#include <binary_messenger.h>
#include <method_channel.h>
#include <method_result.h>

#include <memory>

namespace ihs::osgi {

// Platform-channel front end for the OSGi bridge.
//
// One instance per engine, registered like any other internal plugin. It is a
// thin adapter: argument decoding and channel replies live here, all state and
// ordering live in BridgeRegistry.
//
// The channel exists only to bootstrap. A bundle uses it once, to hand over the
// two things native code cannot obtain on its own -- the address of Dart's
// NativeApi.initializeApiDLData, and its own receive port -- and gets the
// framework isolate's port posted back to that receive port. Everything after
// that is SendPort traffic between isolates, off the platform thread, with no
// channel on the hot path.
class OsgiBridgePlugin {
 public:
  static constexpr char kChannelName[] = "dev.osgi/bridge";

  // Bootstrap. Args (map):
  //   role           "framework" | "bundle"
  //   dl_data        int, NativeApi.initializeApiDLData.address
  //   port           int, ReceivePort.sendPort.nativePort
  //   symbolic_name  string, bundles only
  // Replies true, or an error code below.
  static constexpr char kMethodInit[] = "init";

  // Release a bundle's registration so the same symbolic name can re-register
  // after a restart. Args: symbolic_name (string). Replies true if it was
  // known.
  static constexpr char kMethodShutdown[] = "shutdown";

  // Error codes returned to Dart.
  static constexpr char kErrorBadArgs[] = "bad_arguments";
  static constexpr char kErrorDartApi[] = "dart_api_unavailable";
  static constexpr char kErrorRejected[] = "rejected";

  explicit OsgiBridgePlugin(flutter::BinaryMessenger* messenger);

 private:
  static void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  std::unique_ptr<flutter::MethodChannel<>> channel_;
};

}  // namespace ihs::osgi
