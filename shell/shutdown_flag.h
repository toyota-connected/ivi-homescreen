/*
 * Copyright 2024 Toyota Connected North America
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

#include <csignal>

// Process run flag: 1 while the app runs, cleared to 0 (async-signal-safe) by
// the shutdown signal handler in main.cc. App::Run() (the reactor) and the
// legacy App::Loop() dispatch re-check it to stop the loop; the handler also
// writes the MainLoopWaker eventfd so an idle loop wakes immediately.
//
// Defined in app.cc (the shell library), NOT main.cc, so the library is
// self-contained: the app unit test links app.cc without main.cc and must be
// able to resolve `running`.
extern volatile sig_atomic_t running;
