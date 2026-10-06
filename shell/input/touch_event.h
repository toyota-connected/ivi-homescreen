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

#include <cstdint>

#include <shell/platform/embedder/embedder.h>

namespace ihs {

/// One touch contact update within a hardware scan (a touch frame).
///
/// Its own header rather than a member of Engine: the frame accumulator that
/// builds batches of these is worth testing without linking the engine, and
/// Engine::TouchEvent remains an alias so no call site changes.
struct TouchEvent {
  FlutterPointerPhase phase;
  double x;
  double y;
  int32_t device;
};

}  // namespace ihs
