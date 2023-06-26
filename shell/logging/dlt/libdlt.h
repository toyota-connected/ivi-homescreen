/*
 * Copyright 2023 Toyota Connected North America
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

#include <cstdint>

static constexpr char DLT_PACKAGE_MAJOR_VERSION[] = "2";
static constexpr char DLT_PACKAGE_MINOR_VERSION[] = "18";

constexpr int kDltIdSize = 4;

typedef struct {
  char id[kDltIdSize];
  int32_t pos;
  int8_t* p1;
  int8_t* p2;
  uint8_t count;
} DltContext;

typedef enum { TIMESTAMP_AUTO = 0, TIMESTAMP_USER } DltTimestampType;

typedef struct {
  DltContext* handle;
  unsigned char* buffer;
  int32_t size;
  int32_t log_level;
  int32_t trace_status;
  int32_t args_num;
  char* context_description;
  DltTimestampType use_timestamp;
  uint32_t user_timestamp;
  int8_t verbose_mode;
} DltContextData;

typedef enum {
  LOG_DEFAULT = -1,
  LOG_OFF = 0,
  LOG_FATAL = 1,
  LOG_ERROR = 2,
  LOG_WARN = 3,
  LOG_INFO = 4,
  LOG_DEBUG = 5,
  LOG_VERBOSE = 6,
  LOG_NUM_SEVERITIES
} DltLogLevelType;

typedef enum {
  FileSizeErr = -8,
  LoggingDisabled = -7,
  UserBufferFull = -6,
  WrongParameter = -5,
  BufferFull = -4,
  PipeFill = -3,
  PipeErr = -2,
  Err = -1,
  Ok = 0,
  True = 1,
} DltReturnValue;

struct LibDltExports {
  LibDltExports() = default;
  explicit LibDltExports(void* lib);

  DltReturnValue (*CheckLibraryVersion)(const char*, const char*) = nullptr;
  DltReturnValue (*RegisterApp)(const char*, const char*) = nullptr;
  DltReturnValue (*UnregisterApp)() = nullptr;
  DltReturnValue (*RegisterContext)(DltContext*,
                                    const char*,
                                    const char*) = nullptr;
  DltReturnValue (*UnregisterContext)(DltContext*) = nullptr;
  DltReturnValue (*GetAppId)(char*) = nullptr;
  DltReturnValue (*UserLogWriteStart)(DltContext*,
                                      DltContextData*,
                                      DltLogLevelType) = nullptr;
  DltReturnValue (*UserLogWriteFinish)(DltContextData*) = nullptr;
  DltReturnValue (*UserLogWriteString)(DltContextData*, const char*) = nullptr;
  DltReturnValue (*UserLogWriteInt)(DltContextData*, int) = nullptr;
  DltReturnValue (*UserLogWriteInt8)(DltContextData*, int8_t) = nullptr;
  DltReturnValue (*UserLogWriteInt16)(DltContextData*, int16_t) = nullptr;
  DltReturnValue (*UserLogWriteInt32)(DltContextData*, int32_t) = nullptr;
  DltReturnValue (*UserLogWriteInt64)(DltContextData*, int64_t) = nullptr;
};

class LibDlt {
 public:
  static bool IsPresent() { return loadExports() != nullptr; }

  LibDltExports* operator->();

 private:
  static LibDltExports* loadExports();
};

extern LibDlt LibDlt;
