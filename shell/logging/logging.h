// Copyright 2013 The Flutter Authors. All rights reserved.
// Copyright 2023 Toyota Connected North America. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <sstream>

#include "spdlog/cfg/env.h"
#include "spdlog/sinks/callback_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"

#if defined(ENABLE_DLT)
#include "dlt/dlt.h"
#endif

#include "constants.h"

static std::shared_ptr<spdlog::logger> gLogger{};
static std::shared_ptr<
    spdlog::sinks::ansicolor_stdout_sink<spdlog::details::console_mutex>>
    gConsoleSink;

inline void LoggingPrologue() {
  spdlog::cfg::load_env_levels();

#if defined(ENABLE_DLT)
  if (Dlt::IsSupported()) {
    Dlt::Register();
    gLogger = spdlog::callback_logger_mt(
        "primary", [](const spdlog::details::log_msg& msg) {
          switch (msg.level) {
            case SPDLOG_LEVEL_TRACE:
              Dlt::LogString(DltLogLevelType::LOG_VERBOSE, msg.payload.data());
              break;
            case SPDLOG_LEVEL_DEBUG:
              Dlt::LogString(DltLogLevelType::LOG_DEBUG, msg.payload.data());
              break;
            case SPDLOG_LEVEL_INFO:
              Dlt::LogString(DltLogLevelType::LOG_INFO, msg.payload.data());
              break;
            case SPDLOG_LEVEL_WARN:
              Dlt::LogString(DltLogLevelType::LOG_WARN, msg.payload.data());
              break;
            case SPDLOG_LEVEL_ERROR:
              Dlt::LogString(DltLogLevelType::LOG_ERROR, msg.payload.data());
              break;
            case SPDLOG_LEVEL_CRITICAL:
              Dlt::LogString(DltLogLevelType::LOG_FATAL, msg.payload.data());
              break;
            default:
              break;
          }
        });
    spdlog::set_default_logger(gLogger);
    spdlog::set_pattern("%v");
  } else {
#endif
    gConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    gLogger = std::make_shared<spdlog::logger>("primary", gConsoleSink);
    spdlog::set_default_logger(gLogger);
    spdlog::set_pattern("[%H:%M:%S.%f] [%L] %v");
#if defined(ENABLE_DLT)
  }
#endif

  spdlog::flush_on(spdlog::level::err);
  spdlog::flush_every(std::chrono::seconds(kLogFlushInterval));
}

inline void LoggingEpilogue() {
#if defined(ENABLE_DLT)
  if (Dlt::IsSupported()) {
    // switch logger to console, since we are unregistering DLT
    gLogger.reset();
    gConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    gLogger = std::make_shared<spdlog::logger>("post-dlt", gConsoleSink);
    spdlog::register_logger(gLogger);

    Dlt::Unregister();
  }
#endif
}
