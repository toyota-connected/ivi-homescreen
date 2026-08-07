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

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ihs/config.h"
#include "ihs/ihs.h"
#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_semantics.h"
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

/* A trivial C measurement source + filter, only so the registration ABI is
 * exercised as strict C11 (the point of this consumer). They are never bound
 * here — that is the manager's job in a later step. */
static int smoke_src_start(void* ud, IhsMeasSink sink, void* sink_ud) {
  (void)ud;
  (void)sink;
  (void)sink_ud;
  return 1;
}
static void smoke_src_stop(void* ud) {
  (void)ud;
}
static int smoke_flt_state; /* a static object handed back as the instance */
static void* smoke_flt_create(void* ud, const char* config) {
  (void)ud;
  (void)config;
  return &smoke_flt_state; /* any non-NULL instance (object, not a function) */
}
static void smoke_flt_destroy(void* inst) {
  (void)inst;
}
static void smoke_flt_update(void* inst, const IhsMeasurement* m) {
  (void)inst;
  (void)m;
}
static int smoke_flt_estimate(void* inst, uint64_t t_ns, IhsPosition* out) {
  (void)inst;
  (void)t_ns;
  (void)out;
  return 0;
}

/* Push callback for the location subscribe ABI (never fires against a dead
 * port; present only to exercise the ABI as strict C11). */
static void smoke_location_cb(void* ud, const IhsPosition* pos) {
  (void)ud;
  (void)pos;
}

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
  CHECK(ihs_location_latest2(loc, &pos, sizeof(pos)) == 0,
        "latest2 also reports no fix from a dead port");
  CHECK(ihs_location_generation(loc) == 0, "generation 0 before any fix");
  /* Subscribe/clear the push callback and check NULL safety (it never fires
   * against a dead port; this exercises the ABI). */
  ihs_location_set_callback(loc, smoke_location_cb, NULL);
  ihs_location_set_callback(loc, NULL, NULL);               /* clear */
  ihs_location_set_callback(NULL, smoke_location_cb, NULL); /* NULL is safe */
  ihs_location_stop(loc);
  CHECK(ihs_location_latest(NULL, &pos) == 0, "latest(NULL) is safe");
  CHECK(ihs_location_latest2(NULL, &pos, sizeof(pos)) == 0,
        "latest2(NULL) is safe");
  /* latest2 with the old (pre-accuracy) struct size: a smaller out_size must be
   * honored without overrunning, which is the whole point of the accessor.
   * offsetof(has_bearing)+4 is the legacy size; a byte less must be rejected.
   */
  {
    const size_t legacy = offsetof(IhsPosition, has_bearing) + sizeof(int32_t);
    CHECK(ihs_location_latest2(NULL, &pos, legacy) == 0,
          "latest2 accepts the legacy struct size");
    CHECK(ihs_location_latest2(loc, &pos, legacy - 1) == 0,
          "latest2 rejects a below-legacy size");
  }
  CHECK(ihs_location_generation(NULL) == 0, "generation(NULL) is 0");
  ihs_location_stop(NULL); /* must be a safe nop */

  /* Measurement source / filter registry: additive ABI. Register one of each,
   * assert the return codes and the argument-rejection contract. Nothing binds
   * them here; this is the header/ABI C-cleanliness gate for the new surface.
   */
  {
    IhsLocationSourceOps src_ops;
    IhsLocationFilterOps flt_ops;
    memset(&src_ops, 0, sizeof(src_ops));
    src_ops.struct_size = sizeof(src_ops);
    src_ops.start = smoke_src_start;
    src_ops.stop = smoke_src_stop;
    CHECK(ihs_location_register_source("gnss.smoke", &src_ops, NULL) == 1,
          "register a measurement source");
    CHECK(ihs_location_register_source(NULL, &src_ops, NULL) == 0,
          "reject a NULL source key");
    CHECK(ihs_location_register_source("gnss.smoke", NULL, NULL) == 0,
          "reject NULL source ops");

    memset(&flt_ops, 0, sizeof(flt_ops));
    flt_ops.struct_size = sizeof(flt_ops);
    flt_ops.create = smoke_flt_create;
    flt_ops.destroy = smoke_flt_destroy;
    flt_ops.update = smoke_flt_update;
    flt_ops.estimate = smoke_flt_estimate;
    CHECK(ihs_location_register_filter("kalman.smoke", &flt_ops, NULL) == 1,
          "register a filter");
    CHECK(ihs_location_register_filter(NULL, &flt_ops, NULL) == 0,
          "reject a NULL filter key");
  }

  /* Semantics hub and MCP provider surfaces. Both are meant to be consumed by
   * an out-of-tree FFI plugin, so the point here is that they parse and link
   * as strict C11 from an installed package -- the same property ffigen needs.
   * Exercised, not merely included: an unreferenced header would still compile
   * if a declaration were unresolvable at link time. */
  {
    const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
    /* No engine in this process, so there is nothing published. */
    CHECK(snapshot == NULL, "no semantics snapshot without an engine");
    ihs_semantics_release_snapshot(NULL); /* documented as a no-op */

    CHECK(ihs_semantics_register(NULL, NULL) == IHS_SEMANTICS_ERR_INVALID,
          "reject a NULL consumer desc");
    CHECK((IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS &
           IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS) == 0,
          "the automation mask withholds accessibility focus");

    CHECK(ihs_mcp_provider_register(NULL, NULL) == IHS_MCP_ERR_INVALID,
          "reject a NULL provider desc");
    ihs_mcp_provider_unregister(NULL); /* must tolerate NULL */
    CHECK((IHS_MCP_CAP_READ_ONLY & IHS_MCP_CAP_INTERACT) == 0,
          "read-only capability does not permit interaction");
  }

  printf("OK ihs_shared consumer smoke: abi=0x%08x logging=%s\n",
         api->abi_version, api->logging != NULL ? "present" : "absent");
  return 0;
}
