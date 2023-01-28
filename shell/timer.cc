/*
 * @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include "timer.h"
#include "constants.h"

#include <flutter/fml/logging.h>

#include <memory>
#include <string>
#include <cerrno>
#include <cstring>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>

uint32_t EventTimer::watched_fd = 0;
int EventTimer::evfd = -1;

EventTimer::EventTimer(int clock, evtimer_cb callback, void* callback_data)
  : m_callback(callback),
    m_callback_data(callback_data) {
  FML_DLOG(INFO) << "+ EventTimer()";
  if (evfd < 0) {
    // initialize class-global epoll fd
    evfd = epoll_create1(EPOLL_CLOEXEC);
    if (evfd < 0) {
      if (errno != EINVAL) {
        FML_LOG(ERROR) << "Failed to create epoll fd cloexec. " << strerror(errno);
        exit(-1);
      } else {
        // fallback
        evfd = epoll_create(1);
        if (evfd < 0) {
          FML_LOG(ERROR) << "Failed to create epoll fd. " << strerror(errno);
          exit(-1);
        }
      }
    }
  }

  m_timerfd = timerfd_create(clock, TFD_CLOEXEC | TFD_NONBLOCK);
  if (m_timerfd == -1) {
    FML_LOG(ERROR) << "Failed to create timerfd. " << strerror(errno);
    exit(-1);
  }

  watch_timerfd();
  FML_DLOG(INFO) << "- EventTimer()";
}

EventTimer::~EventTimer() {
  FML_DLOG(INFO) << "+ ~EventTimer()";

  unwatch_timerfd();
  close(m_timerfd);
  m_timerfd = -1;

  if (watched_fd == 0)
    close_evfd();

  FML_DLOG(INFO) << "- ~EventTimer()";
}

void EventTimer::close_evfd(void) {
  FML_DLOG(INFO) << "+ EventTimer::close_evfd()";
  close(evfd);
  evfd = -1;
  FML_DLOG(INFO) << "- EventTimer::close_evfd()";
}

void EventTimer::set_timerspec(int32_t rate, int32_t delay) {
  FML_DLOG(INFO) << "+ EventTimer::set_timerspec()";

  int32_t repeat_rate_sec, repeat_rate_nsec, repeat_delay_sec, repeat_delay_nsec = 0;

  if (rate == 0)
    return;

  repeat_rate_sec = rate / 1000;
  rate -= (repeat_rate_sec * 1000);
  repeat_rate_nsec = rate * 1000 * 1000;

  repeat_delay_sec = delay / 1000;
  delay -= (repeat_delay_sec * 1000);
  repeat_delay_nsec = delay * 1000 * 1000;

  m_timerspec.it_interval.tv_sec  = repeat_rate_sec;
  m_timerspec.it_interval.tv_nsec = repeat_rate_nsec;
  m_timerspec.it_value.tv_sec     = repeat_delay_sec;
  m_timerspec.it_value.tv_nsec    = repeat_delay_nsec;

  FML_DLOG(INFO) << "- EventTimer::set_timerspec()";
}

void EventTimer::_arm(int fd, struct itimerspec *timerspec) {
  auto ret = timerfd_settime(fd, 0, timerspec, NULL);
  if (ret < 0) {
    FML_LOG(ERROR) << "Failed to release timer. " << strerror(errno);
    exit(-1);
  }
}

void EventTimer::arm(void) {
  FML_DLOG(INFO) << "+ EventTimer::arm()";

  _arm(m_timerfd, &m_timerspec);

  FML_DLOG(INFO) << "- EventTimer::arm()";
}

void EventTimer::disarm(void) {
  FML_DLOG(INFO) << "+ EventTimer::disarm()";

  struct itimerspec timerspec = {};

  _arm(m_timerfd, &timerspec);

  FML_DLOG(INFO) << "- EventTimer::disarm()";
}

void EventTimer::_watch_fd(int fd, uint32_t events, struct timer_task* task)
{
  struct epoll_event ep;

  if (evfd < 0) {
    FML_LOG(ERROR) << "Unexpected call _watch_fd(). Ignored.";
    return;
  }

  ep.events = events;
  ep.data.ptr = task;
  epoll_ctl(evfd, EPOLL_CTL_ADD, fd, &ep);
  watched_fd++;
}

void EventTimer::watch_timerfd(void) {
  FML_DLOG(INFO) << "+ EventTimer::watch_timerfd()";
  m_task.run = this->run;
  m_task.data = reinterpret_cast<void*>(this);

  _watch_fd(m_timerfd, EPOLLIN, &m_task);
  FML_DLOG(INFO) << "- EventTimer::watch_timerfd()";
}

void EventTimer::_unwatch_fd(int fd)
{
  if (evfd < 0) {
    FML_LOG(ERROR) << "Unexpected call _unwatch_fd(). Ignored.";
    return;
  }
  epoll_ctl(evfd, EPOLL_CTL_DEL, fd, NULL);
  watched_fd--;
}

void EventTimer::unwatch_timerfd(void) {
  FML_DLOG(INFO) << "+ EventTimer::unwatch_timerfd()";
  _unwatch_fd(m_timerfd);
  FML_DLOG(INFO) << "- EventTimer::unwatch_timerfd()";
}

void EventTimer::wait_event(void) {
  struct epoll_event ep[10];

  auto ready = epoll_wait(evfd, ep, sizeof(ep) / sizeof(ep[0]), 0);
  for (auto i = 0; i < ready; i++) {
    struct timer_task* task = reinterpret_cast<struct timer_task*>(ep[i].data.ptr);
    task->run(task, ep[i].events);
  }
}

void EventTimer::run(struct timer_task* task, uint32_t events) {
  uint64_t event;
  EventTimer* timer = reinterpret_cast<EventTimer*>(task->data);

  if (events != EPOLLIN)
    FML_LOG(ERROR) << "Found Unexpected timerfd events: " <<
      std::showbase << std::hex << events << std::noshowbase;

  if (!(events & EPOLLIN))
    return;

  if (read(timer->m_timerfd, &event, sizeof(event)) != sizeof(event)) {
    if (errno != EAGAIN)
      FML_LOG(ERROR) << "Failed to read timer. " << strerror(errno);
      return;
  }

  timer->m_callback(timer->m_callback_data);
}

