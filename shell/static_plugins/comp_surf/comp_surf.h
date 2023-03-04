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

#include <flutter_embedder.h>

class CompositorSurfacePlugin {
 public:
  static constexpr char kChannelName[] = "comp_surf";

  static constexpr char kMethodCreate[] = "create";
  static constexpr char kMethodDispose[] = "dispose";

  static constexpr char kArgView[] = "view";
  static constexpr char kArgModule[] = "module";
  static constexpr char kArgAssetsPath[] = "assets_path";
  static constexpr char kCacheFolder[] = "cache_folder";
  static constexpr char kMiscFolder[] = "misc_folder";
  static constexpr char kArgType[] = "type";
  static constexpr char kArgZOrder[] = "z_order";
  static constexpr char kArgSync[] = "sync";
  static constexpr char kArgWidth[] = "width";
  static constexpr char kArgHeight[] = "height";
  static constexpr char kArgX[] = "x";
  static constexpr char kArgY[] = "y";
  static constexpr char kSurfaceIndex[] = "index";

  static constexpr char kParamTypeEgl[] = "egl";
  static constexpr char kParamTypeVulkan[] = "vulkan";
  static constexpr char kParamZOrderAbove[] = "above";
  static constexpr char kParamZOrderBelow[] = "below";
  static constexpr char kParamSyncSync[] = "sync";
  static constexpr char kParamSyncDeSync[] = "de-sync";

  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);
};
