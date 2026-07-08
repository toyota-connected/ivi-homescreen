#pragma once

#include <config/common.h>

#include <map>
#include <string>
#include <vector>

class CrashHandler {
 public:
  // |config_path| is the full path to the config.toml holding the process-level
  // [sentry] table (the --config master file when given, else the first
  // bundle's config.toml).
  explicit CrashHandler(const std::string& config_path);

  ~CrashHandler();

#if INTEGRATION_TEST_CRASH_HANDLER
  static void trigger_crash();
#endif

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
