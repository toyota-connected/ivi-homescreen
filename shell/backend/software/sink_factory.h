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

#include <memory>
#include <string_view>

#include "backend/software/surface_sink.h"

// Parse a sink spec into an ISurfaceSink. Spec syntax:
//
//   none                       NoneSink (default).
//   memory                     MemorySink (in-process snapshot store).
//   file:<path-pattern>        FileSink. Pattern can contain a printf
//                              "%d" / "%05d" / etc. for the frame
//                              index; a literal path with no "%"
//                              captures only the first frame.
//
// Unrecognized specs log a warn and return NoneSink so the embedder
// never refuses to start because of a CI typo.
std::unique_ptr<ISurfaceSink> MakeSinkFromSpec(std::string_view spec);

// Convenience: read IVI_SW_SINK from the environment; falls back to
// "none" when the env var is unset or empty.
std::unique_ptr<ISurfaceSink> MakeSinkFromEnv();
