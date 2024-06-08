/*
 * Copyright 2020 Toyota Connected North America
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

#include "config.h"

#if !defined(NDEBUG)
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#else
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_OFF
#endif
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#if ENABLE_DLT
#include "dlt/dlt.h"
#endif

class Logging {
 public:
  Logging();
  ~Logging();

 private:
  std::shared_ptr<spdlog::logger> m_logger{};
  std::shared_ptr<
      spdlog::sinks::ansicolor_stdout_sink<spdlog::details::console_mutex>>
      m_console_sink;
};
