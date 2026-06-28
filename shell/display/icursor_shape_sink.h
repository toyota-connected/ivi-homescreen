// Copyright 2020-2026 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

namespace homescreen {

// A backend cursor that can change its sprite to a named XCursor shape. The
// backend registers the active cursor with its display so
// IDisplay::ActivateSystemCursor can retarget the live cursor regardless of
// which concrete cursor (HW plane, GL-composited, software) is in use.
class ICursorShapeSink {
 public:
  virtual ~ICursorShapeSink() = default;

  // Set the cursor sprite to the given XCursor name (resolved against the
  // active theme), e.g. "default", "pointer", "text". A null name requests
  // hiding the cursor. Returns false if the shape could not be loaded.
  //
  // Called from the platform thread; the actual sprite swap may be deferred to
  // the backend's render thread, so implementations must be thread-safe.
  virtual bool SetShape(const char* xcursor_name) = 0;
};

}  // namespace homescreen
