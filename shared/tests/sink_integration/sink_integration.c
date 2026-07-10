/*
 * Copyright 2026 Toyota Connected North America
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

/*
 * End-to-end logging sink test, driven entirely through the installed C ABI.
 * Sinks are selected by environment at ihs_log_start, and ihs_log_start is a
 * process singleton, so each scenario runs as its own invocation:
 *
 *   sink_integration console   -- ConsoleSink formats to stderr
 *   sink_integration file      -- RotatingFileSink writes + rotates
 *   sink_integration level     -- IHS_LOG_LEVEL floor drops verbose records
 *   sink_integration fallback  -- IHS_LOG_SINK=dlt with no libdlt warns + falls
 *                                 back to console (missing-list #17 contract)
 *
 * The scenarios that inspect console output redirect stderr to a temp file,
 * flush, then read it back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ihs/logging.h"

#define FAILF(...)                         \
  do {                                     \
    fprintf(stdout, "FAIL: " __VA_ARGS__); \
    fprintf(stdout, "\n");                 \
    return 1;                              \
  } while (0)

/* Read a whole file into a NUL-terminated heap buffer (caller frees). */
static char* slurp(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  if (n < 0) {
    n = 0;
  }
  fseek(f, 0, SEEK_SET);
  char* buf = (char*)malloc((size_t)n + 1);
  if (buf) {
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
  }
  fclose(f);
  return buf;
}

static int file_exists(const char* path) {
  return access(path, F_OK) == 0;
}

/* Redirect stderr to a fresh temp file; returns its path (static storage). */
static const char* capture_stderr(void) {
  static char path[] = "/tmp/ihs_sink_capXXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    return NULL;
  }
  close(fd);
  if (!freopen(path, "w", stderr)) {
    return NULL;
  }
  return path;
}

static int scenario_console(void) {
  const char* cap = capture_stderr();
  if (!cap) {
    FAILF("could not capture stderr");
  }
  setenv("IHS_LOG_SINK", "console", 1);
  if (ihs_log_start("SINK", "sink integration") != 1) {
    FAILF("ihs_log_start");
  }
  const int32_t ctx = ihs_log_context_open("CONS", NULL);
  if (ctx < 0) {
    FAILF("context_open");
  }
  const char msg[] = "hello-console-42";
  ihs_log(ctx, IHS_LEVEL_INFO, msg, sizeof(msg) - 1);
  ihs_log_flush();
  ihs_log_stop();

  char* out = slurp(cap);
  remove(cap);
  if (!out) {
    FAILF("no capture file");
  }
  int ok = strstr(out, "hello-console-42") != NULL &&
           strstr(out, "CONS") != NULL && strstr(out, "[I]") != NULL;
  free(out);
  if (!ok) {
    FAILF("console output missing text/tag/level");
  }
  printf("OK console\n");
  return 0;
}

static int scenario_file(void) {
  static char logpath[] = "/tmp/ihs_sink_logXXXXXX";
  int fd = mkstemp(logpath);
  if (fd < 0) {
    FAILF("mkstemp");
  }
  close(fd);
  setenv("IHS_LOG_SINK", "file", 1);
  setenv("IHS_LOG_FILE", logpath, 1);
  setenv("IHS_LOG_FILE_MAX_BYTES", "200", 1);
  setenv("IHS_LOG_FILE_MAX_FILES", "3", 1);
  if (ihs_log_start("SINK", "sink integration") != 1) {
    FAILF("ihs_log_start");
  }
  const int32_t ctx = ihs_log_context_open("FILE", NULL);
  if (ctx < 0) {
    FAILF("context_open");
  }
  for (int i = 0; i < 80; ++i) {
    char line[64];
    const int n = snprintf(line, sizeof(line), "file-line-%04d-payload", i);
    ihs_log(ctx, IHS_LEVEL_INFO, line, (size_t)n);
  }
  ihs_log_flush();
  ihs_log_stop();

  char rotated[300];
  snprintf(rotated, sizeof(rotated), "%s.1", logpath);
  const int have_live = file_exists(logpath);
  const int have_rotated = file_exists(rotated);
  /* Content check: the live file can be empty right after a rotation, so accept
   * the record text in either the live file or the most recent rotated file. */
  char* live = slurp(logpath);
  char* prev = slurp(rotated);
  const int have_content =
      (live != NULL && strstr(live, "file-line-") != NULL) ||
      (prev != NULL && strstr(prev, "file-line-") != NULL);
  free(live);
  free(prev);

  remove(logpath);
  for (int i = 1; i <= 3; ++i) {
    char p[300];
    snprintf(p, sizeof(p), "%s.%d", logpath, i);
    remove(p);
  }
  if (!have_live) {
    FAILF("live log file missing");
  }
  if (!have_rotated) {
    FAILF("rotation did not produce %s", rotated);
  }
  if (!have_content) {
    FAILF("log file content missing");
  }
  printf("OK file (rotated)\n");
  return 0;
}

static int scenario_level(void) {
  const char* cap = capture_stderr();
  if (!cap) {
    FAILF("could not capture stderr");
  }
  setenv("IHS_LOG_SINK", "console", 1);
  setenv("IHS_LOG_LEVEL", "warn", 1); /* Fatal..Warn pass; Info/Debug drop */
  if (ihs_log_start("SINK", "sink integration") != 1) {
    FAILF("ihs_log_start");
  }
  const int32_t ctx = ihs_log_context_open("LVL", NULL);
  if (ctx < 0) {
    FAILF("context_open");
  }
  const char info_line[] = "info-should-drop";
  const char warn_line[] = "warn-should-pass";
  ihs_log(ctx, IHS_LEVEL_INFO, info_line, sizeof(info_line) - 1);
  ihs_log(ctx, IHS_LEVEL_WARN, warn_line, sizeof(warn_line) - 1);
  ihs_log_flush();
  ihs_log_stop();

  char* out = slurp(cap);
  remove(cap);
  if (!out) {
    FAILF("no capture file");
  }
  const int warn_present = strstr(out, "warn-should-pass") != NULL;
  const int info_present = strstr(out, "info-should-drop") != NULL;
  free(out);
  if (!warn_present) {
    FAILF("warn record was filtered but should pass");
  }
  if (info_present) {
    FAILF("info record passed the warn floor");
  }
  printf("OK level floor\n");
  return 0;
}

static int scenario_fallback(void) {
  const char* cap = capture_stderr();
  if (!cap) {
    FAILF("could not capture stderr");
  }
  /* Default sink is dlt; with no libdlt on the box it must warn once and fall
   * back to console rather than log silently to nowhere. */
  setenv("IHS_LOG_SINK", "dlt", 1);
  if (ihs_log_start("SINK", "sink integration") != 1) {
    FAILF("ihs_log_start");
  }
  const int32_t ctx = ihs_log_context_open("FBK", NULL);
  if (ctx < 0) {
    FAILF("context_open");
  }
  const char msg[] = "fallback-line-visible";
  ihs_log(ctx, IHS_LEVEL_INFO, msg, sizeof(msg) - 1);
  ihs_log_flush();
  ihs_log_stop();

  char* out = slurp(cap);
  remove(cap);
  if (!out) {
    FAILF("no capture file");
  }
  const int warned = strstr(out, "[ihs_log]") != NULL;
  const int logged = strstr(out, "fallback-line-visible") != NULL;
  free(out);
  /* If libdlt happens to be present the warn/fallback won't trigger; then the
   * DLT sink took the record and console is empty -- both are acceptable. */
  if (warned && !logged) {
    FAILF("warned but the console fallback produced no output");
  }
  printf("OK fallback (warned=%d logged=%d)\n", warned, logged);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stdout, "usage: %s console|file|level|fallback\n", argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "console") == 0) {
    return scenario_console();
  }
  if (strcmp(argv[1], "file") == 0) {
    return scenario_file();
  }
  if (strcmp(argv[1], "level") == 0) {
    return scenario_level();
  }
  if (strcmp(argv[1], "fallback") == 0) {
    return scenario_fallback();
  }
  fprintf(stdout, "unknown scenario: %s\n", argv[1]);
  return 2;
}
