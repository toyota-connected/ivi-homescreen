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

#include "display/software_display.h"

SoftwareDisplay::SoftwareDisplay(const int32_t width,
                                 const int32_t height,
                                 const double refresh_rate_hz)
    : width_(width), height_(height), refresh_rate_hz_(refresh_rate_hz) {}

void SoftwareDisplay::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  view_controller_state_ = state;
}
