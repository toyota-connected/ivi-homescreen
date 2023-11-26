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

#include <flutter/standard_method_codec.h>
#include <shell/platform/embedder/embedder.h>

class FlutterView;

class CompositorRegionPlugin {
 public:
  static constexpr char kChannelName[] = "comp_region";

  static constexpr char kMethodMask[] = "mask";

  /* array of group index references */
  static constexpr char kArgClear[] = "clear";

  /* array */
  static constexpr char kArgGroups[] = "groups";

  /* mask type - input or opaque */
  static constexpr char kArgGroupType[] = "type";

  /* rectangle array */
  static constexpr char kArgRegions[] = "regions";

  /* members of rectangle array */
  static constexpr char kArgRegionX[] = "x";
  static constexpr char kArgRegionY[] = "y";
  static constexpr char kArgRegionWidth[] = "width";
  static constexpr char kArgRegionHeight[] = "height";

  typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
  } REGION_T;

  /**
   * @brief Handle a platform message from the Flutter engine.
   * @param[in] message The message from the Flutter engine.
   * @param[in] userdata The user data.
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

 private:
  /**
   * @brief Handle a platform message per flutter view groups.
   * @param[in] groups flutter view groups.
   * @param[in] view flutter view.
   * @return flutter::EncodableValue
   * @retval encoded value
   * @relation
   * flutter
   */
  static flutter::EncodableValue HandleGroups(flutter::EncodableList& groups,
                                              FlutterView* view);
  /**
   * @brief Clear flutter view groups.
   * @param[in] types flutter view group types.
   * @param[in] view flutter view.
   * @return void
   * @relation
   * flutter
   */
  static void ClearGroups(flutter::EncodableList& types, FlutterView* view);
};
