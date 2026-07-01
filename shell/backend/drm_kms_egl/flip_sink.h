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

// Completion target for one DRM PAGE_FLIP_EVENT. Every atomic/legacy commit
// passes its sink as the flip's user_data; the card's single page-flip handler
// routes the event back to the exact committer by calling OnFlipEvent(). This
// lets several compositors (one per output) share one backend's flip
// reader even though a v2 flip event carries no CRTC id -- each flip returns to
// whoever armed it, identified only by the pointer it registered.
class IFlipSink {
 public:
  virtual ~IFlipSink() = default;
  virtual void OnFlipEvent(unsigned int tv_sec, unsigned int tv_usec) = 0;
};
