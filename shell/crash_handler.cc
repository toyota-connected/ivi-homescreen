#include "crash_handler.h"
#include <filesystem>

#include "utils.h"

#include <config/common.h>
#include <cstdlib>
#include <cstring>

#define TOML_EXCEPTIONS 0
#include <tomlplusplus/toml.hpp>

#include "sentry.h"

auto invalid_mem = reinterpret_cast<void*>(1);

void CrashHandler::trigger_crash() {
  memset(invalid_mem, 1, 100);
}

CrashHandler::SentryConfig CrashHandler::LoadConfig(
    const std::string& bundle_path) {
  SentryConfig config;

  // Defaults
  config.release = "";
  config.env = kSentryEnvDefault;

  // DSN from environment variable
  const char* dsn_env = std::getenv("SENTRY_DSN");
  config.dsn = dsn_env ? dsn_env : "";

  std::filesystem::path toml_path;
  if (bundle_path.empty()) {
    spdlog::warn("Bundle path is empty, using defaults");
    return config;
  }

  toml_path = bundle_path;
  toml_path /= kViewConfigToml;

  if (!std::filesystem::exists(toml_path)) {
    spdlog::warn("Global config file not found at {}, using defaults",
                 toml_path.string());
    return config;
  }

  // Parse TOML file
  auto result = toml::parse_file(toml_path.string());
  if (!result) {
    spdlog::error("TOML parsing failed: {}", toml_path.string());
    return config;
  }

  auto& tbl = result.table();

  // Extract [sentry] section
  if (auto* sentry_section = tbl.get("sentry")) {
    if (auto* release = sentry_section->as_table()->get("release"))
      config.release = release->value_or("");
    if (auto* env = sentry_section->as_table()->get("env"))
      config.env = env->value_or(kSentryEnvDefault);

    // Extract [sentry.tags] subsection
    if (auto* tags_node = sentry_section->as_table()->get("tags")) {
      if (auto* tags_tbl = tags_node->as_table()) {
        for (auto& [key, val] : *tags_tbl) {
          if (auto str_val = val.value<std::string>())
            config.tags[std::string(key)] = *str_val;
        }
      }
    }

    // Extract [sentry] attachments array
    if (auto* arr_node = sentry_section->as_table()->get("attachments")) {
      if (auto* arr = arr_node->as_array()) {
        for (auto& val : *arr) {
          if (auto str_val = val.value<std::string>())
            config.attachments.push_back(*str_val);
        }
      }
    }
  }

  return config;
}

CrashHandler::CrashHandler(const std::string& bundle_path) {
  config_ = LoadConfig(bundle_path);

  sentry_options_t* options = sentry_options_new();
  sentry_options_set_dsn(options, config_.dsn.c_str());
  auto home_path = Utils::GetConfigHomePath();
  std::filesystem::path db_path = home_path;
  db_path /= ".sentry";
  sentry_options_set_handler_path(options, kCrashpadBinaryPath);
  sentry_options_set_database_path(options, db_path.c_str());

  sentry_options_set_release(options, config_.release.c_str());
  sentry_options_set_environment(options, config_.env.c_str());

  // Apply attachments
  for (auto& attachment : config_.attachments) {
    std::filesystem::path attachment_path(attachment);
    if (exists(attachment_path)) {
      sentry_options_add_attachment(options, attachment_path.c_str());
    }
  }

  sentry_options_set_symbolize_stacktraces(options, true);
  sentry_options_set_debug(options, 0);

  sentry_init(options);

  // Apply tags (must be called after sentry_init)
  for (auto& [tag_name, tag_val] : config_.tags) {
    sentry_set_tag(tag_name.c_str(), tag_val.c_str());
  }
}

CrashHandler::~CrashHandler() {
  sentry_close();
}
