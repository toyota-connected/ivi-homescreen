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

#include "config/common.h"

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

#include "spdlog/cfg/env.h"  // support for loading levels from the environment variable
#include "spdlog/spdlog-inl.h"
#if ENABLE_DLT
#include "spdlog/sinks/callback_sink.h"
#endif
#include "spdlog/sinks/ringbuffer_sink.h"

class Logging {
 public:
  Logging() {
#if ENABLE_DLT
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
                Dlt::LogSizedString(DltLogLevelType::LOG_INFO,
                                    msg.payload.data(),
                                    static_cast<uint16_t>(msg.payload.size()));
                break;
              case SPDLOG_LEVEL_WARN:
                Dlt::LogSizedString(DltLogLevelType::LOG_WARN,
                                    msg.payload.data(),
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
#if ENABLE_DLT
    }
#endif

#ifndef NDEBUG
    m_logger->set_level(spdlog::level::debug);
#endif
    spdlog::flush_on(spdlog::level::err);
    spdlog::flush_every(std::chrono::seconds(kLogFlushInterval));
    spdlog::cfg::load_env_levels();
  }

  ~Logging() {
#if ENABLE_DLT
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

 private:
  std::shared_ptr<spdlog::logger> m_logger{};
  std::shared_ptr<
      spdlog::sinks::ansicolor_stdout_sink<spdlog::details::console_mutex>>
      m_console_sink;
};

#define DLOG_DEBUG SPDLOG_DEBUG
#define DLOG_TRACE SPDLOG_TRACE
#define DLOG_CRITICAL SPDLOG_CRITICAL

#define LOG_INFO spdlog::info
#define LOG_DEBUG spdlog::debug
#define LOG_WARN spdlog::warn
#define LOG_ERROR spdlog::error
#define LOG_TRACE spdlog::trace
#define LOG_CRITICAL spdlog::critical
