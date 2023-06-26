#include "crash_handler.h"

#include "fml/paths.h"
#include "utils.h"

#include <cstring>

#include "sentry.h"


void* invalid_mem = (void*)1;

void CrashHandler::CrashHandler::trigger_crash() {
  memset((char*)invalid_mem, 1, 100);
}

CrashHandler::CrashHandler() {
  sentry_options_t* options = sentry_options_new();
  sentry_options_set_dsn(options, kCrashHandlerDsn);
  auto home_path = Utils::GetConfigHomePath();
  auto db_path = fml::paths::JoinPaths({home_path, ".sentry"});
  sentry_options_set_database_path(options, db_path.c_str());
  sentry_options_set_release(options, kCrashHandlerRelease);
  sentry_options_set_symbolize_stacktraces(options, true);
  sentry_options_set_debug(options, 0);
  sentry_init(options);
}

CrashHandler::~CrashHandler() {
  sentry_close();
}
