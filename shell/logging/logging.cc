// Copyright 2013 The Flutter Authors. All rights reserved.
// Copyright 2023 Toyota Connected North America. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstring>
#include <iostream>

#include "logging.h"

#include "dlt/dlt.h"

namespace {

const char* const kLogSeverityNames[DltLogLevelType::LOG_NUM_SEVERITIES + 1] = {
    "DEFAULT", "OFF", "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};

const char* GetNameForLogSeverity(DltLogLevelType severity) {
  if (severity >= DltLogLevelType::LOG_DEFAULT &&
      severity < DltLogLevelType::LOG_NUM_SEVERITIES) {
    return kLogSeverityNames[severity + 1];
  }
  return "UNKNOWN";
}

const char* StripDots(const char* path) {
  while (strncmp(path, "../", 3) == 0) {
    path += 3;
  }
  return path;
}

const char* StripPath(const char* path) {
  auto* p = strrchr(path, '/');
  if (p) {
    return p + 1;
  }
  return path;
}

}  // namespace

LogMessage::LogMessage(DltLogLevelType severity,
                       const char* file,
                       int line,
                       const char* condition)
    : severity_(severity), file_(file), line_(line) {
  if (LibDlt::IsPresent()) {
    return;
  }

  stream_ << "[";
  if (severity >= DltLogLevelType::LOG_DEFAULT &&
      severity < LOG_NUM_SEVERITIES) {
    stream_ << GetNameForLogSeverity(severity);
  } else {
    stream_ << "VERBOSE" << -severity;
  }
  stream_ << ":"
          << (severity > DltLogLevelType::LOG_INFO ? StripDots(file_)
                                                   : StripPath(file_))
          << "(" << line_ << ")] ";

  if (condition) {
    stream_ << "Check failed: " << condition << ". ";
  }
}

LogMessage::~LogMessage() {
  if (!LibDlt::IsPresent()) {
    stream_ << std::endl;
  }

  Dlt::LogString(severity_, stream_.str().c_str());

  if (severity_ == DltLogLevelType::LOG_FATAL) {
    KillProcess();
  }
}

bool ShouldCreateLogMessage(DltLogLevelType severity) {
  return severity != DltLogLevelType::LOG_OFF;
}

void KillProcess() {
  abort();
}
