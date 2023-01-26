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

#pragma once

#include <memory>
#include <string>

#include <sys/timerfd.h>
#include <sys/epoll.h>

struct timer_task {
  void (*run)(struct timer_task* task, uint32_t events);
  void* data;
};

typedef void (*evtimer_cb)(void *);

class EventTimer {
  public:
    /**
    * @brief Constructor of EventTimer
    * @param[in] The type of clock for a timer
    * @param[in] callback a callback to run when a timer alarmes
    * @param[in] callback_data an arument for a callback
    * @return EventTimer
    * @retval an instance of EventTimer
    * @relation
    * internal
    */
    explicit EventTimer(int clock, evtimer_cb callback, void* callback_data);
    ~EventTimer();
    EventTimer(const EventTimer&) = delete;
    const EventTimer& operator=(const EventTimer&) = delete;

    static uint32_t watched_fd;
    static int evfd;

    int m_timerfd;
    struct itimerspec m_timerspec;

    struct timer_task m_task;
    evtimer_cb m_callback;
    void* m_callback_data;

    /**
    * @brief clock epoll fd
    * @return void
    * @relation
    * internal
    */
    static void close_evfd(void);

    /**
    * @brief specify timer spec
    * @param[in] rate repeat rate
    * @param[in] delay repeat delay
    * @return void
    * @relation
    * internal
    */
    void set_timerspec(int32_t rate, int32_t delay);

    /**
    * @brief run when a timer alarmes
    * @return void
    * @relation
    * internal
    */
    void arm(void);
    /**
    * @brief run when a timer is stopped
    * @return void
    * @relation
    * internal
    */
    void disarm(void);

    /**
    * @brief register timerfd
    * @return void
    * @relation
    * internal
    */
    void watch_timerfd(void);
    /**
    * @brief unregister timerfd
    * @return void
    * @relation
    * internal
    */
    void unwatch_timerfd(void);

    /**
    * @brief run callback
    * @param[in] task the set of data for callback
    * @param[in] events triggered events on epoll()
    * @return void
    * @relation
    * internal
    */
    static void run(struct timer_task* task, uint32_t events);

    /**
    * @brief check epoll events
    * @return void
    * @relation
    * internal
    */
    static void wait_event(void);

private:
    /**
    * @brief an internal func for arm/disarm
    * @param[in] fd timer fd
    * @param[in] timerspec timerspec
    * @return void
    * @relation
    * internal
    */
    static void _arm(int fd, struct itimerspec* timerspec);
    /**
    * @brief an internal func for watch_timerfd
    * @param[in] fd timer fd
    * @param[in] events observed events
    * @param[in] task the set of data for callback
    * @return void
    * @relation
    * internal
    */
    static void _watch_fd(int fd, uint32_t events, struct timer_task* task);
    /**
    * @brief an internal func for unwatch_timerfd
    * @param[in] fd timer fd
    * @return void
    * @relation
    * internal
    */
    static void _unwatch_fd(int fd);
};
