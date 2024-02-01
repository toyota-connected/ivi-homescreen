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

#include "logging.h"

#include <spdlog/cfg/env.h>
#if defined(ENABLE_DLT)
#include <spdlog/sinks/callback_sink.h>
#endif

#include <constants.h>

Logging::Logging() {
#if defined(ENABLE_DLT)
  if (Dlt::IsSupported()) {
    Dlt::Register();
    m_logger = spdlog::callback_logger_mt(
        "primary", [](const spdlog::details::log_msg& msg) {
          switch (msg.level) {
            case SPDLOG_LEVEL_TRACE:
              Dlt::LogSizedString(DltLogLevelType::LOG_VERBOSE,
                                  msg.payload.data(),
                                  static_cast<uint16_t>(msg.payload.size()));
              break;
            case SPDLOG_LEVEL_DEBUG:
              Dlt::LogSizedString(DltLogLevelType::LOG_DEBUG,
                                  msg.payload.data(),
                                  static_cast<uint16_t>(msg.payload.size()));
              break;
            case SPDLOG_LEVEL_INFO:
              Dlt::LogSizedString(DltLogLevelType::LOG_INFO, msg.payload.data(),
                                  static_cast<uint16_t>(msg.payload.size()));
              break;
            case SPDLOG_LEVEL_WARN:
              Dlt::LogSizedString(DltLogLevelType::LOG_WARN, msg.payload.data(),
                                  static_cast<uint16_t>(msg.payload.size()));
              break;
            case SPDLOG_LEVEL_ERROR:
              Dlt::LogSizedString(DltLogLevelType::LOG_ERROR,
                                  msg.payload.data(),
                                  static_cast<uint16_t>(msg.payload.size()));
              break;
            case SPDLOG_LEVEL_CRITICAL:
              Dlt::LogSizedString(DltLogLevelType::LOG_FATAL,
                                  msg.payload.data(),
                                  static_cast<uint16_t>(msg.payload.size()));
              break;
            default:
              break;
          }
        });
    spdlog::set_default_logger(m_logger);
    spdlog::set_pattern("%v");
  } else {
#endif
    m_console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    m_logger = std::make_shared<spdlog::logger>("primary", m_console_sink);
    spdlog::set_default_logger(m_logger);
    spdlog::set_pattern("[%H:%M:%S.%f] [%L] %v");
#if defined(ENABLE_DLT)
  }
#endif

  spdlog::flush_on(spdlog::level::err);
  spdlog::flush_every(std::chrono::seconds(kLogFlushInterval));
  spdlog::cfg::load_env_levels();
}

Logging::~Logging() {
#if defined(ENABLE_DLT)
  if (Dlt::IsSupported()) {
    // switch logger to console, since we are unregistering DLT
    m_logger.reset();
    m_console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    m_logger = std::make_shared<spdlog::logger>("post-dlt", m_console_sink);
    spdlog::register_logger(m_logger);

    Dlt::Unregister();
  }
#endif
}