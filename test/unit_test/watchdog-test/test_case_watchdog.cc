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

#include <chrono>
#include <thread>
#include "gtest/gtest.h"
#include "watchdog.h"

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogConstruct_Lv1Normal001
Use Case Name: Construction
Test Summary：Test default construction creates valid Watchdog
***************************************************************/

TEST(HomescreenWatchdogConstruct, Lv1Normal001) {
  // call target API
  Watchdog watchdog;

  // check: construction succeeds — implicit if we reach here
  // Destructor will clean up without crash
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogConstruct_Lv1Normal002
Use Case Name: Construction
Test Summary：Test parameterized construction with custom interval
***************************************************************/

TEST(HomescreenWatchdogConstruct, Lv1Normal002) {
  // set parameters
  const auto interval = std::chrono::microseconds(100'000);  // 100ms

  // call target API
  Watchdog watchdog(interval);

  // check: construction succeeds with custom interval
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogLifecycle_Lv1Normal001
Use Case Name: Start/Pet/Stop
Test Summary：Test normal start, pet, stop lifecycle — no timeout
***************************************************************/

TEST(HomescreenWatchdogLifecycle, Lv1Normal001) {
  // set parameters — 500ms interval, pet every 100ms
  const auto interval = std::chrono::microseconds(500'000);
  Watchdog watchdog(interval);

  // call target API
  watchdog.start();

  for (int i = 0; i < 5; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    watchdog.pet();
  }

  watchdog.stop();

  // check: reached here without timeout — lifecycle completed normally
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogPet_Lv1Normal001
Use Case Name: Pet
Test Summary：Test pet extends deadline — survives longer than
              a single interval by petting repeatedly
***************************************************************/

TEST(HomescreenWatchdogPet, Lv1Normal001) {
  // set parameters — 200ms interval, run for 1 second
  const auto interval = std::chrono::microseconds(200'000);
  Watchdog watchdog(interval);

  // call target API
  watchdog.start();

  // pet 10 times over ~1 second — each pet resets the 200ms deadline
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    watchdog.pet();
  }

  watchdog.stop();

  // check: survived ~1 second with 200ms timeout — pet extends deadline
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogStop_Lv1Normal001
Use Case Name: Stop
Test Summary：Test double stop safety — calling stop() twice
***************************************************************/

TEST(HomescreenWatchdogStop, Lv1Normal001) {
  // set parameters
  const auto interval = std::chrono::microseconds(500'000);
  Watchdog watchdog(interval);

  // call target API
  watchdog.start();
  watchdog.pet();
  watchdog.stop();
  watchdog.stop();  // second stop — must not crash

  // check: no crash on double stop
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogStop_Lv1Normal002
Use Case Name: Stop
Test Summary：Test stop before start — stop() on never-started
              watchdog does not crash
***************************************************************/

TEST(HomescreenWatchdogStop, Lv1Normal002) {
  // set parameters
  Watchdog watchdog;

  // call target API — stop without ever starting
  watchdog.stop();

  // check: no crash
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogDestructor_Lv1Normal001
Use Case Name: RAII
Test Summary：Test destructor joins thread — no resource leak
              when destroyed while watchdog is running
***************************************************************/

TEST(HomescreenWatchdogDestructor, Lv1Normal001) {
  // set parameters — long interval so watchdog stays alive
  const auto interval = std::chrono::microseconds(5'000'000);

  // call target API — create and destroy in inner scope
  {
    Watchdog watchdog(interval);
    watchdog.start();
    watchdog.pet();
    // destructor called here — should stop and join thread
  }

  // check: no crash, no dangling thread
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdogTimeoutDeathTest_Lv1Abnormal001
Use Case Name: Timeout
Test Summary：Test timeout triggers process exit when not petted.
              start() calls pet() once, setting deadline to
              now + interval. Without further pets, the watchdog
              thread detects expiry and calls exit(EXIT_FAILURE).
***************************************************************/

TEST(HomescreenWatchdogTimeoutDeathTest, Lv1Abnormal001) {
  EXPECT_EXIT(
      {
        // Use short interval for fast test
        Watchdog watchdog(std::chrono::microseconds(50'000));  // 50ms
        watchdog.start();
        // Do NOT pet — let the watchdog timeout.
        // Sleep long enough for the watchdog thread to detect expiry.
        // The thread polls every 250ms (kDefaultSleepTime), so 500ms
        // provides sufficient margin.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      },
      ::testing::ExitedWithCode(EXIT_FAILURE), "");
}
