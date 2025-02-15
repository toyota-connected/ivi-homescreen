#pragma once

#include <string>
#include "sentry.h"

class CrashHandler {
 public:
  CrashHandler();

  ~CrashHandler();

  static void trigger_crash();
  static const char* get_dsn();
  static void set_sentry_attachments(sentry_options_t* options); 
  static void set_sentry_tags();

  CrashHandler(const CrashHandler&) = delete;
  CrashHandler& operator=(const CrashHandler&) = delete;
};
