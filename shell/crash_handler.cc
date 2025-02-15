#include "crash_handler.h"

#include "utils.h"

#include <cstring>
#include <fstream>

#include "sentry.h"

auto invalid_mem = reinterpret_cast<void*>(1);

void CrashHandler::trigger_crash() {
  memset(invalid_mem, 1, 100);
}

const char* CrashHandler::get_dsn() {
  const auto dsn_env = getenv("SENTRY_DSN");
  const char* dsn;

  if (dsn_env && *dsn_env) {
    dsn = dsn_env;
  } else {
    dsn = kCrashHandlerDsn;
  }

  return dsn;
}

void CrashHandler::set_sentry_attachments(sentry_options_t* options) {
  const auto attachment_env = getenv("SENTRY_ATTACHMENTS");
  std::string attachments(kCrashpadAttachments);
  if (attachment_env && *attachment_env) {
    attachments.append(",");
    attachments.append(attachment_env);
  }
  std::stringstream ss(attachments);
  std::string attachment;

  while (getline(ss, attachment, ',')) {
    std::filesystem::path attachment_path(attachment);
    if (exists(attachment_path)) {
      sentry_options_add_attachment(options, attachment_path.c_str());
    }
  }
}

void CrashHandler::set_sentry_tags() {
  const auto tags_env = getenv("SENTRY_TAGS");
  std::string tags(kCrashpadTags);

  if (tags_env && *tags_env) {
    tags.append(",");
    tags.append(tags_env);
  }
  std::stringstream ss(tags);
  std::string tag;
  while (getline(ss, tag, ',')) {
    size_t del = tag.find("=", 0);
    std::string tag_name = tag.substr(0,del);
    size_t tag_val_size = tag.size() - tag_name.size() - 1;
    std::string tag_val = tag.substr(del+1, tag_val_size);
    sentry_set_tag(tag_name.c_str(), tag_val.c_str());
  }

}

CrashHandler::CrashHandler() {
  sentry_options_t* options = sentry_options_new();
  sentry_options_set_dsn(options, get_dsn());
  auto home_path = Utils::GetConfigHomePath();
  std::filesystem::path db_path = home_path;
  db_path /= ".sentry";
  sentry_options_set_handler_path(options, kCrashpadBinaryPath);
  sentry_options_set_database_path(options, db_path.c_str());

  const auto release_env = getenv("SENTRY_RELEASE");
  if (release_env && *release_env) {
    sentry_options_set_release(options, release_env);
  }
  else {
    sentry_options_set_release(options, kCrashHandlerRelease);
  }

  set_sentry_attachments(options);

  sentry_options_set_symbolize_stacktraces(options, true);
  sentry_options_set_debug(options, 0);

  sentry_init(options);
  set_sentry_tags();
}

CrashHandler::~CrashHandler() {
  sentry_close();
}
