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
 * Out-of-tree consumer smoke test for ihs_shared. Built from an empty tree
 * against the INSTALLED CMake package (find_package(ivi-homescreen-shared)),
 * exactly as a Dart FFI plugin's native library would consume it. Compiled as
 * strict C11 so it doubles as a header C-cleanliness check. Exercises the
 * version handshake and each capability sub-table end to end.
 */

#include <stdio.h>
#include <string.h>

#include "ihs/config.h"
#include "ihs/ihs.h"
#include "ihs/location.h"
#include "ihs/logging.h"
#include "ihs/trace.h"

#define CHECK(cond, msg)           \
  do {                             \
    if (!(cond)) {                 \
      printf("FAIL: %s\n", (msg)); \
      return 1;                    \
    }                              \
  } while (0)

int main(void) {
  /* Version handshake. */
  const IhsApi* api = ihs_get_api(IHS_SHARED_ABI_VERSION);
  CHECK(api != NULL, "ihs_get_api returned NULL for the matching major");
  CHECK(api->struct_size == sizeof(IhsApi), "IhsApi struct_size mismatch");
  CHECK(ihs_get_api((IHS_SHARED_ABI_MAJOR + 1u) << 16) == NULL,
        "a newer major should be rejected");

  /* Tracing: always present. Emitting is a safe nop with no sink installed. */
  CHECK(api->trace != NULL, "trace sub-table absent");
  api->trace->instant("consumer-smoke");
  IHS_TRACE_INSTANT("consumer-smoke-macro");

  /* Config: publish a snapshot (builder is part of the ABI) and read it back.
   */
  CHECK(api->config != NULL, "config sub-table absent");
  IhsConfigBuilder* builder = ihs_config_builder_new(1);
  ihs_config_builder_set_str(builder, IHS_CONFIG_GLOBAL, "app_id", "smoke");
  ihs_config_builder_set_int(builder, 0, "width", 1280);
  const uint64_t generation = ihs_config_publish(builder);
  CHECK(generation >= 1, "publish did not return a generation");

  const IhsConfigSnapshot* snap = api->config->acquire();
  CHECK(snap != NULL, "acquire after publish returned NULL");
  char app_id[64];
  CHECK(api->config->get_str(snap, IHS_CONFIG_GLOBAL, "app_id", app_id,
                             sizeof(app_id), NULL),
        "get_str app_id");
  CHECK(strcmp(app_id, "smoke") == 0, "app_id value mismatch");
  int64_t width = 0;
  CHECK(api->config->get_int(snap, 0, "width", &width) && width == 1280,
        "get_int width");
  api->config->release(snap);

  /* Logging: present only in an ENABLE_DLT build. Generic surface — a context
   * opens and logs even with no DLT daemon (records fall to the console sink),
   * so ihs_log_start must succeed and the context must be valid. */
  if (api->logging != NULL) {
    CHECK(api->logging->start("SMOK", "consumer smoke") == 1, "ihs_log_start");
    const int32_t ctx = api->logging->context_open("SMOK", NULL);
    CHECK(ctx >= 0, "context_open valid without a DLT daemon");
    /* Fast-path gate (additive table field): Info passes the default floor,
     * Off never does. */
    CHECK(api->logging->enabled(ctx, IHS_LEVEL_INFO) == 1, "enabled(Info)");
    CHECK(api->logging->enabled(ctx, IHS_LEVEL_OFF) == 0, "enabled(Off)");
    const char line[] = "consumer smoke line";
    api->logging->log(ctx, IHS_LEVEL_INFO, line, sizeof(line) - 1);
    api->logging->flush();
    api->logging->stop();
  }

  /* Location: exercise the ihs_location_* ABI without needing gpsd or geoclue
   * present. Point gpsd at a dead port so no fix is ever produced, and assert
   * the contract: a handle is returned, no fix, generation 0, and stop frees
   * cleanly. This guards the ABI/headers against regressions in CI. */
  IhsLocationService* loc =
      ihs_location_start(IHS_LOCATION_GPSD, "127.0.0.1:9");
  CHECK(loc != NULL, "ihs_location_start returned NULL");
  IhsPosition pos;
  CHECK(ihs_location_latest(loc, &pos) == 0,
        "no fix expected from a dead port");
  CHECK(ihs_location_generation(loc) == 0, "generation 0 before any fix");
  ihs_location_stop(loc);
  CHECK(ihs_location_latest(NULL, &pos) == 0, "latest(NULL) is safe");
  CHECK(ihs_location_generation(NULL) == 0, "generation(NULL) is 0");
  ihs_location_stop(NULL); /* must be a safe nop */

  printf("OK ihs_shared consumer smoke: abi=0x%08x logging=%s\n",
         api->abi_version, api->logging != NULL ? "present" : "absent");
  return 0;
}
