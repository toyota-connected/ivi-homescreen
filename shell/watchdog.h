/*
 * Copyright 2024 Toyota Connected North America
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

#ifndef SHELL_WATCHDOG_H_
#define SHELL_WATCHDOG_H_

#include <atomic>
#include <chrono>
#include <thread>

/**
 * @brief The default timeout interval for the Watchdog.
 */
class Watchdog {
 public:
  static constexpr uint64_t kDefaultTimeout = 5'000'000;
  static constexpr uint64_t kDefaultSleepTime = 250;

  /**
   * @brief Default constructor for the Watchdog class.
   *
   * This constructor initializes the Watchdog object with the default timeout
   * interval. It also sets the stop flag to false and logs the interval set for
   * the Watchdog Timer.
   */
  explicit Watchdog();

  /**
   * @brief Destructor for the Watchdog class
   *
   * The destructor for the Watchdog class. It stops the watchdog timer, if it
   * is running, and performs any necessary cleanup. If the watchdog thread is
   * joinable, it will stop the thread and notify the system (if
   * BUILD_SYSTEMD_WATCHDOG is defined) that it is stopping.
   *
   * @see Watchdog::stop(), sd_notify()
   */
  ~Watchdog();

  /**
   * @brief Starts the watchdog timer.
   *
   * This method starts the watchdog timer by creating a separate thread that
   * continuously checks if the timeout has occurred. If the timeout has
   * occurred, it logs a critical message and takes appropriate action based on
   * the build configuration. The watchdog timer will continue running until the
   * stop() method is called or the timeout occurs.
   *
   * @see stop()
   */
  void start();

  /**
   * @brief Stops the watchdog timer.
   *
   * This method stops the watchdog timer if it is running. It sets the stop
   * flag to true, which will cause the watchdog thread to exit. If the watchdog
   * thread is joinable, it will wait for the thread to complete before
   * returning.
   *
   * @see Watchdog::start()
   */
  void stop();

  /**
   * @brief Resets the watchdog timer deadline to the current time plus the
   * interval.
   *
   * This method updates the deadline for the watchdog timer by calculating the
   * current time plus the interval. It also notifies the system (if
   * BUILD_SYSTEMD_WATCHDOG is defined) that the watchdog is active.
   *
   * @see start(), sd_notify()
   */
  void pet();

  Watchdog(const Watchdog&) = delete;

  const Watchdog& operator=(const Watchdog&) = delete;

 private:
  std::chrono::microseconds interval_;
  std::thread watchdog_thread_;
  std::atomic<bool> stop_flag_;
  std::chrono::steady_clock::time_point deadline_;
};

#endif  // SHELL_WATCHDOG_H_