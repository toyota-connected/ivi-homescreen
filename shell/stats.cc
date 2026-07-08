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

#include "stats.h"

#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "logging/logging.h"

void Stats::getSelfStats(ProcessStats& stats) {
  std::ifstream statFile("/proc/self/stat");
  if (!statFile.is_open()) {
    spdlog::error("Failed to open /proc/self/stat");
    exit(EXIT_FAILURE);
  }

  std::string line;
  std::getline(statFile, line);
  statFile.close();

  std::istringstream iss(line);
  std::vector<std::string> fields((std::istream_iterator<std::string>(iss)),
                                  std::istream_iterator<std::string>());

  stats.num_threads = std::stol(fields[19]);

  if (fields.size() >= 24) {
    stats.virtual_memory = std::stod(fields[22]);
    stats.resident_set_size = std::stod(fields[23]);
    long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;

    stats.virtual_memory = stats.virtual_memory / 1024.0;
    stats.resident_set_size =
        stats.resident_set_size * static_cast<double>(page_size_kb);
  }
}
