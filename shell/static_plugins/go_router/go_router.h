/*
 * Copyright 2020 Toyota Connected North America
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

#include <shell/platform/embedder/embedder.h>
#include <string>

class GoRouter {
 public:
  static constexpr char kChannelName[] = "flutter/navigation";

  /**
   * @brief Callback function for platform messages about navigation
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

 private:
  /// Removes the topmost Flutter instance, presenting what was before it.
  ///
  /// On Android, removes this activity from the stack and returns to the
  /// previous activity.
  ///
  /// On iOS, calls `popViewControllerAnimated:` if the root view controller is
  /// a `UINavigationController`, or `dismissViewControllerAnimated:completion:`
  /// if the top view controller is a `FlutterViewController`.
  ///
  /// The optional `animated` parameter is ignored on all platforms
  /// except iOS where it is an argument to the aforementioned methods.
  ///
  /// This method should be preferred over calling `dart:io`'s [exit] method, as
  /// the latter may cause the underlying platform to act as if the application
  /// had crashed.
  static constexpr char kMethodSystemNavigatorPop[] =
      "SystemNavigator.pop";
  /// Selects the single-entry history mode.
  ///
  /// On web, this switches the browser history model to one that only tracks a
  /// single entry, so that calling [routeInformationUpdated] replaces the
  /// current entry.
  ///
  /// Currently, this is ignored on other platforms.
  ///
  /// See also:
  ///
  ///  * [selectMultiEntryHistory], which enables the browser history to have
  ///    multiple entries.
  static constexpr char kSelectSingleEntryHistory[] =
      "selectSingleEntryHistory";
  /// Selects the multiple-entry history mode.
  ///
  /// On web, this switches the browser history model to one that tracks all
  /// updates to [routeInformationUpdated] to form a history stack. This is the
  /// default.
  ///
  /// Currently, this is ignored on other platforms.
  ///
  /// See also:
  ///
  ///  * [selectSingleEntryHistory], which forces the history to only have one
  ///  entry.
  static constexpr char kSelectMultiEntryHistory[] = "selectMultiEntryHistory";
  /// Notifies the platform for a route information change.
  ///
  /// On web, this method behaves differently based on the single-entry or
  /// multiple-entries history mode. Use the [selectSingleEntryHistory] and
  /// [selectMultiEntryHistory] to toggle between modes.
  ///
  /// For single-entry mode, this method replaces the current URL and state in
  /// the current history entry. The flag `replace` is ignored.
  ///
  /// For multiple-entries mode, this method creates a new history entry on top
  /// of the current entry if the `replace` is false, thus the user will be on a
  /// new history entry as if the user has visited a new page, and the browser
  /// back button brings the user back to the previous entry. If `replace` is
  /// true, this method only updates the URL and the state in the current
  /// history entry without pushing a new one.
  ///
  /// This method is ignored on other platforms.
  ///
  /// The `replace` flag defaults to false.
  static constexpr char kRouteInformationUpdated[] = "routeInformationUpdated";
};
