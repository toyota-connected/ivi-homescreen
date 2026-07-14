#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

#include "watchdog.h"

static constexpr char kSourceRoot[] = SOURCE_ROOT_DIR;

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Abnormal001
Use Case Name: Pre-initialization
Test Summary：Test getInstance() before init() throws std::runtime_error
***************************************************************/

TEST(HomescreenWatchdog, Lv1Abnormal001) {
  // call target API before init — must throw
  EXPECT_THROW(Watchdog::getInstance(), std::runtime_error);
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test init() with a valid config path completes without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal001) {
  // set parameters
  const std::string config_path =
      std::string(kSourceRoot) + "/files/watchdog_config.toml";

  // call target API
  EXPECT_NO_THROW(Watchdog::init(&config_path));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal002
Use Case Name: Configuration
Test Summary：Test getTimeoutMs() returns the value loaded from config
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal002) {
  // call target API
  const uint64_t timeout_ms = Watchdog::getInstance().getTimeoutMs();

  // check timeout matches watchdog_config.toml value
  EXPECT_EQ(10000u, timeout_ms);
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal003
Use Case Name: Initialization
Test Summary：Test init() called a second time is a no-op (does not throw)
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal003) {
  const std::string config_path =
      std::string(kSourceRoot) + "/files/watchdog_config.toml";

  // call target API again
  EXPECT_NO_THROW(Watchdog::init(&config_path));

  // timeout should remain unchanged — second init is ignored
  EXPECT_EQ(10000u, Watchdog::getInstance().getTimeoutMs());
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal004
Use Case Name: Source naming
Test Summary：Test setSourceName() for a known source completes without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal004) {
  // call target API
  EXPECT_NO_THROW(Watchdog::getInstance().setSourceName(
      WATCHDOG_SOURCE_MAIN_THREAD, "main"));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal005
Use Case Name: Source naming
Test Summary：Test setSourceName() called again on the same source overwrites
              without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal005) {
  // call target API with a different name for the same source
  EXPECT_NO_THROW(Watchdog::getInstance().setSourceName(
      WATCHDOG_SOURCE_MAIN_THREAD, "main_thread"));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal006
Use Case Name: Source monitoring
Test Summary：Test start() for an unmonitored source adds it without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal006) {
  // call target API
  EXPECT_NO_THROW(Watchdog::getInstance().start(WATCHDOG_SOURCE_MAIN_THREAD));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal007
Use Case Name: Source monitoring
Test Summary：Test pet() on an active source resets its timer without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal007) {
  // WATCHDOG_SOURCE_MAIN_THREAD was started in Lv1Normal006
  EXPECT_NO_THROW(Watchdog::getInstance().pet(WATCHDOG_SOURCE_MAIN_THREAD));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Abnormal002
Use Case Name: Source monitoring
Test Summary：Test pet() on a source that was never started does not throw
              (emits a warning internally)
***************************************************************/

TEST(HomescreenWatchdog, Lv1Abnormal002) {
  // WATCHDOG_SOURCE_RENDER_THREAD has not been started
  EXPECT_NO_THROW(Watchdog::getInstance().pet(WATCHDOG_SOURCE_RENDER_THREAD));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal008
Use Case Name: Source monitoring
Test Summary：Test stop() on an active source removes it without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal008) {
  // set parameters: start render thread first
  Watchdog::getInstance().start(WATCHDOG_SOURCE_RENDER_THREAD);

  // call target API
  EXPECT_NO_THROW(Watchdog::getInstance().stop(WATCHDOG_SOURCE_RENDER_THREAD));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Abnormal003
Use Case Name: Source monitoring
Test Summary：Test stop() on a source that is not active does not throw
              (emits a warning internally)
***************************************************************/

TEST(HomescreenWatchdog, Lv1Abnormal003) {
  // WATCHDOG_SOURCE_RENDER_THREAD was stopped in Lv1Normal008
  EXPECT_NO_THROW(Watchdog::getInstance().stop(WATCHDOG_SOURCE_RENDER_THREAD));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal009
Use Case Name: Source monitoring
Test Summary：Test start() on multiple sources simultaneously does not throw
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal009) {
  // call target API for both predefined sources
  EXPECT_NO_THROW(Watchdog::getInstance().start(WATCHDOG_SOURCE_MAIN_THREAD));
  EXPECT_NO_THROW(Watchdog::getInstance().start(WATCHDOG_SOURCE_RENDER_THREAD));
}

/****************************************************************
Test Case Name.Test Name： HomescreenWatchdog_Lv1Normal010
Use Case Name: Shutdown
Test Summary：Test shutdown() stops the watchdog thread cleanly without throwing
***************************************************************/

TEST(HomescreenWatchdog, Lv1Normal010) {
  // call target API
  EXPECT_NO_THROW(Watchdog::getInstance().shutdown());
}
