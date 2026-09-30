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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>

#include <shell/platform/embedder/embedder.h>

// Which platform views are in the scene Flutter presents, frame by frame.
//
// A platform view leaves the scene when a frame no longer composites it --
// Offstage, scrolled off, or covered by a route that stops painting it -- and
// is still alive: nobody disposes it. Its plugin should pause then (a video,
// a camera, a Wayland client told xdg_toplevel.suspended), which is what
// set_suspended promises. Presented() diffs each frame's platform-view layers
// against the last and calls `notify(id, true)` for each view that left and
// `notify(id, false)` for each it had suspended that came back. Views that
// never left get no calls, so a new view is not "resumed" on its first frame.
//
// Presented() runs on the raster thread; Suspended() may be read from any.
class PlatformViewScene {
 public:
  using Notify = std::function<void(int64_t id, bool suspended)>;

  explicit PlatformViewScene(Notify notify);

  // The layers of a frame about to be presented.
  void Presented(const FlutterLayer** layers, size_t count);

  // Whether view @p id is suspended for having left the scene.
  [[nodiscard]] bool Suspended(int64_t id) const;

 private:
  Notify notify_;
  mutable std::mutex mu_;
  std::unordered_set<int64_t> in_scene_;
  std::unordered_set<int64_t> suspended_;
  // Reused across frames.
  std::unordered_set<int64_t> scratch_;
};
