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

#ifndef SHELL_STATS_H_
#define SHELL_STATS_H_

/**
 * @class Stats
 * @brief A class to gather and store process statistics.
 */
class Stats {
 public:
  /**
   * @struct ProcessStats
   * @brief A structure to hold various statistics about a process.
   */
  struct ProcessStats {
    long num_threads;          ///< Number of threads in the process.
    double virtual_memory;     ///< Virtual memory size (KB).
    double resident_set_size;  ///< Resident set size (KB).
  };

  /**
   * @brief Gathers statistics about the current process.
   * @param stats A reference to a ProcessStats structure to store the gathered
   * statistics.
   */
  static void getSelfStats(ProcessStats& stats);
};

#endif  // SHELL_STATS_H_
