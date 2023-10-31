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

class FileSelector {
 public:
  static constexpr char kChannelName[] =
      "plugins.flutter.dev/file_selector_linux";

  /**
   * @brief Callback function for platform messages about isolate
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

 private:
  static constexpr char kMethodOpenFile[] = "openFile";
  static constexpr char kGetSavePath[] = "getSavePath";
  static constexpr char kGetDirectoryPath[] = "getDirectoryPath";

  static constexpr char kArgInitialDirectory[] = "initialDirectory";
  static constexpr char kArgConfirmButtonText[] = "confirmButtonText";
  static constexpr char kArgAcceptedTypeGroups[] = "acceptedTypeGroups";

  static constexpr char kArgTypeGroupLabel[] = "label";
  static constexpr char kArgTypeGroupExtensions[] = "extensions";
  static constexpr char kArgTypeGroupMime[] = "mimeTypes";
  static constexpr char kArgMultiple[] = "multiple";
  static constexpr char kArgSuggestedName[] = "suggestedName";
};
