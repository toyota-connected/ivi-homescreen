// Copyright 2013 The Flutter Authors. All rights reserved.
// Copyright 2023 Toyota Connected North America. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <sstream>

#include "dlt/dlt.h"
#include "flutter/fml/macros.h"

class LogMessageVoid {
 public:
  void operator&(std::ostream&) {}
};

class LogMessage {
 public:
  LogMessage(DltLogLevelType severity,
             const char* file,
             int line,
             const char* condition);
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  std::ostringstream stream_;
  const DltLogLevelType severity_;
  const char* file_;
  const int line_;

  FML_DISALLOW_COPY_AND_ASSIGN(LogMessage);
};

// Gets the FML_VLOG default verbosity level.
int GetVlogVerbosity();

// Returns true if |severity| is at or above the current minimum log level.
// LOG_FATAL and above is always true.
bool ShouldCreateLogMessage(DltLogLevelType severity);

[[noreturn]] void KillProcess();

#define LOG_STREAM(severity)                                               \
  LogMessage(DltLogLevelType::LOG_##severity, __FILE__, __LINE__, nullptr) \
      .stream()

#define LAZY_STREAM(stream, condition) \
  !(condition) ? (void)0 : LogMessageVoid() & (stream)

#define EAT_STREAM_PARAMETERS(ignored) \
  true || (ignored)                    \
      ? (void)0                        \
      : LogMessageVoid() &             \
            LogMessage(DltLogLevelType::LOG_FATAL, 0, 0, nullptr).stream()

#define LOG_IS_ON(severity) \
  (ShouldCreateLogMessage(DltLogLevelType::LOG_##severity))

#define LOG(severity) LAZY_STREAM(LOG_STREAM(severity), LOG_IS_ON(severity))

#define CHECK(condition)                                                     \
  LAZY_STREAM(                                                               \
      LogMessage(DltLogLevelType::LOG_FATAL, __FILE__, __LINE__, #condition) \
          .stream(),                                                         \
      !(condition))

#define VLOG_IS_ON(verbose_level) ((verbose_level) <= GetVlogVerbosity())

// The VLOG macros log with negative verbosities.
#define VLOG_STREAM(verbose_level) \
  LogMessage(-verbose_level, __FILE__, __LINE__, nullptr).stream()

#define VLOG(verbose_level) \
  LAZY_STREAM(VLOG_STREAM(verbose_level), VLOG_IS_ON(verbose_level))

#ifndef NDEBUG
#define DLOG(severity) LOG(severity)
#define DCHECK(condition) CHECK(condition)
#else
#define DLOG(severity) EAT_STREAM_PARAMETERS(true)
#define DCHECK(condition) EAT_STREAM_PARAMETERS(condition)
#endif

#define UNREACHABLE()                          \
  {                                            \
    LOG(ERROR) << "Reached unreachable code."; \
    KillProcess();                             \
  }
