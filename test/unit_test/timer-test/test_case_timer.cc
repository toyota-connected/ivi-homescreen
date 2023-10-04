#include <stdexcept>
#include "gtest/gtest.h"
#include "timer.h"
#include <fcntl.h>

bool call_cbfunc = false;
static int cb_data = 123;

void event_timer_callback(void* data) {
  int* p_data = reinterpret_cast<int*>(data);
  int int_data = static_cast<int>(*p_data);
  EXPECT_EQ(cb_data, int_data);
  call_cbfunc = true;
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerWatchTimerfd_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test watch_timerfd with normal evfd value
***************************************************************/

TEST(HomescreenTimerWatchTimerfd, Lv1Normal001) {
  // set parameters
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  int fd_count = timer.watched_fd;

  // call target API
  timer.watch_timerfd();

  // check watched_fd increased
  EXPECT_EQ(fd_count +1, timer.watched_fd);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerWatchTimerfd_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test watch_timerfd with invalid evfd value
***************************************************************/

TEST(HomescreenTimerWatchTimerfd, Lv1Abnormal001) {
  // set parameters
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  timer.close_evfd();
  int fd_count = timer.watched_fd;

  // call target API
  timer.watch_timerfd();

  // check watched_fd has not changed
  EXPECT_EQ(fd_count, timer.watched_fd);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerUnwatchTimerfd_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test unwatch_timerfd with normal evfd value
***************************************************************/

TEST(HomescreenUntimerWatchTimerfd, Lv1Normal001) {
  // set parameters
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  int fd_count = timer.watched_fd;

  // call target API
  timer.unwatch_timerfd();

  // check watched_fd decreased
  EXPECT_EQ(fd_count -1, timer.watched_fd);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerUnwatchTimerfd_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test unwatch_timerfd with invalid evfd value
***************************************************************/

TEST(HomescreenUntimerWatchTimerfd, Lv1Abnormal001) {
  // set parameters
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  timer.close_evfd();
  int fd_count = timer.watched_fd;

  // call target API
  timer.unwatch_timerfd();

  // check watched_fd has not changed
  EXPECT_EQ(fd_count, timer.watched_fd);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerArm_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test arm function timer set functionality
***************************************************************/

TEST(HomescreenUntimerArm, Lv1Normal001) {
  // set parameters
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  timer.set_timerspec(40, 400);

  // call target API
  timer.arm();

  // check timerspec is set
  struct itimerspec timerspec;
  int ret_gettime = timerfd_gettime(timer.m_timerfd, &timerspec);

  EXPECT_EQ(0, ret_gettime);
  EXPECT_EQ(0, timerspec.it_interval.tv_sec);
  EXPECT_EQ(40*1000*1000, timerspec.it_interval.tv_nsec);
  EXPECT_EQ(0, timerspec.it_value.tv_sec);
  // delay nsec is smaller because it takes into account time passage
  EXPECT_GT(400*1000*1000, timerspec.it_value.tv_nsec);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerDisarm_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test disarm function timer reset functionality
***************************************************************/

TEST(HomescreenUntimerDisarm, Lv1Normal001) {
  // set parameters
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  timer.set_timerspec(40, 400);
  timer.arm(); // set time

  // call target API
  timer.disarm();

  // check timerspec is all zero
  struct itimerspec timerspec;
  int ret_gettime = timerfd_gettime(timer.m_timerfd, &timerspec);

  EXPECT_EQ(0, ret_gettime);
  EXPECT_EQ(0, timerspec.it_interval.tv_sec);
  EXPECT_EQ(0, timerspec.it_interval.tv_nsec);
  EXPECT_EQ(0, timerspec.it_value.tv_sec);
  EXPECT_EQ(0, timerspec.it_value.tv_nsec);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerRun_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test run without O_NONBLOCK filediscripter flag
***************************************************************/

TEST(HomescreenTimerRun, Lv1Normal001) {
  // set parameters
  call_cbfunc = false;
  void* data = reinterpret_cast<void*>(&cb_data);

  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  timer.set_timerspec(40, 400);
  timer.arm();

  // remove O_NONBLOCK setting from file discripter flags
  fcntl(timer.m_timerfd, F_SETFL, O_CLOEXEC);

  // call target API
  timer_task task;
  task.data = reinterpret_cast<void*>(&timer);
  timer.run(&task, EPOLLIN);

  // check the callback func is called
  EXPECT_EQ(true, call_cbfunc);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerRun_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test run with O_NONBLOCK filediscripter flag
***************************************************************/

TEST(HomescreenTimerRun, Lv1Normal002) {
  // set parameters
  call_cbfunc = false;
  void* data = reinterpret_cast<void*>(&cb_data);

  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);
  timer.set_timerspec(40, 400);
  timer.arm();

  // set file discripter O_NONBLOCK flag
  fcntl(timer.m_timerfd, F_SETFL, O_CLOEXEC | O_NONBLOCK);

  // call target API
  timer_task task;
  task.data = reinterpret_cast<void*>(&timer);
  timer.run(&task, EPOLLIN);

  // check the callback func is not called
  EXPECT_EQ(false, call_cbfunc);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTimerRun_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test run with invalid timerfd event
***************************************************************/

TEST(HomescreenTimerRun, Lv1Abnormal001) {
  // set parameters
  call_cbfunc = false;
  void* data = reinterpret_cast<void*>(&cb_data);
  EventTimer timer(CLOCK_MONOTONIC, event_timer_callback, data);

  // call target API
  timer_task task;
  task.data = reinterpret_cast<void*>(&timer);
  timer.run(&task, EPOLLERR);

  // check the callback func is not called
  EXPECT_EQ(false, call_cbfunc);
}
