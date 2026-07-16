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
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include "display/idisplay.h"
#include "display/output.h"
#include "display/output_provider.h"

// GMock mock for IDisplay.  Use NiceMock<MockIDisplay> in tests that only
// care about GetOutputProvider() so uninteresting calls don't generate warnings.
class MockIDisplay : public IDisplay {
 public:
  MOCK_METHOD(void, StartEvents, (), (override));
  MOCK_METHOD(void, StopEvents, (), (override));
  MOCK_METHOD(int, PollEvents, (), (const, override));
  MOCK_METHOD(void,
              SetViewControllerState,
              (FlutterDesktopViewControllerState*),
              (override));
  MOCK_METHOD(double, GetRefreshRate, (uint32_t), (const, override));
  MOCK_METHOD(double, GetMaxRefreshRate, (), (const, override));
  MOCK_METHOD(int32_t, GetBufferScale, (uint32_t), (const, override));
  MOCK_METHOD((std::pair<int32_t, int32_t>),
              GetVideoModeSize,
              (uint32_t),
              (const, override));
  MOCK_METHOD(bool,
              ActivateSystemCursor,
              (int32_t, const std::string&),
              (const, override));
  MOCK_METHOD(bool, HasRepeatTimer, (), (const, override));
  MOCK_METHOD(homescreen::IOutputProvider*, GetOutputProvider, (), (override));
};

// GMock mock for homescreen::IOutputProvider.
class MockIOutputProvider : public homescreen::IOutputProvider {
 public:
  MOCK_METHOD(std::vector<homescreen::OutputInfo>,
              EnumerateOutputs,
              (),
              (const, override));
  MOCK_METHOD(void,
              SetOutputListener,
              (homescreen::IOutputListener*),
              (override));
  MOCK_METHOD(bool, SupportsHotplug, (), (const, override));
};
