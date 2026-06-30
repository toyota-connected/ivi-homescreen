#pragma once

#include <map>
#include <string>
#include <vector>
#include "sentry.h"

class CrashHandler {
 public:
  // |config_path| is the full path to the config.toml holding the process-level
  // [sentry] table (the --config master file when given, else the first
  // bundle's config.toml).
  explicit CrashHandler(const std::string& config_path);

  ~CrashHandler();

  static void trigger_crash();

  CrashHandler(const CrashHandler&) = delete;
  CrashHandler& operator=(const CrashHandler&) = delete;

 private:
  struct SentryConfig {
    std::string dsn;
    std::string release;
    std::string env;
    std::map<std::string, std::string> tags;
    std::vector<std::string> attachments;
  };

  static SentryConfig LoadConfig(const std::string& config_path);

  SentryConfig config_;
  bool initialized_ = false;
};
