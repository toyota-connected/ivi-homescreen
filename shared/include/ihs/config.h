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
 * ihs_shared configuration read surface (C ABI). The shell publishes a
 * flattened, generation-stamped snapshot of the active configuration; plugins
 * acquire a snapshot (which stays valid until released even across a config
 * replacement) and read typed values by flattened key. Config is not immutable
 * over the process lifetime — it is replaced per generation — so a plugin holds
 * a snapshot and can cheaply poll ihs_config_generation to notice a change.
 * See docs/PLUGIN_ABI.md.
 */

#ifndef IHS_CONFIG_H_
#define IHS_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque, reference-counted configuration snapshot. */
typedef struct IhsConfigSnapshot IhsConfigSnapshot;

/* A view index of -1 addresses global (non-per-view) keys. */
#define IHS_CONFIG_GLOBAL (-1)

/*
 * Acquire the current snapshot with its reference count incremented, or NULL if
 * none has been published. Must be released with ihs_config_release. The
 * snapshot stays valid until released even if a newer generation is published.
 */
IHS_EXPORT const IhsConfigSnapshot* ihs_config_acquire(void);

/* Release a snapshot previously returned by ihs_config_acquire. */
IHS_EXPORT void ihs_config_release(const IhsConfigSnapshot* snapshot);

/* The snapshot's generation. Increases by one per published snapshot, so a
 * plugin can poll for "did the config change" cheaply. */
IHS_EXPORT uint64_t ihs_config_generation(const IhsConfigSnapshot* snapshot);

/* Number of views (per-view key scopes) in the snapshot. */
IHS_EXPORT size_t ihs_config_view_count(const IhsConfigSnapshot* snapshot);

/*
 * Typed getters. `view` selects the per-view scope, or IHS_CONFIG_GLOBAL.
 * `key` is a flattened dotted path (e.g. "width", "view.drm_rotation"). Return
 * 1 when the key is present with the requested type, 0 otherwise.
 *
 * ihs_config_get_str copies up to `cap` bytes (including the NUL terminator)
 * into `out` and, when `needed` is non-NULL, sets it to the string length
 * excluding the NUL. Pass out=NULL, cap=0 to query the required length.
 */
IHS_EXPORT int ihs_config_get_str(const IhsConfigSnapshot* snapshot,
                                  int32_t view,
                                  const char* key,
                                  char* out,
                                  size_t cap,
                                  size_t* needed);
IHS_EXPORT int ihs_config_get_int(const IhsConfigSnapshot* snapshot,
                                  int32_t view,
                                  const char* key,
                                  int64_t* out);
IHS_EXPORT int ihs_config_get_bool(const IhsConfigSnapshot* snapshot,
                                   int32_t view,
                                   const char* key,
                                   int* out);
IHS_EXPORT int ihs_config_get_double(const IhsConfigSnapshot* snapshot,
                                     int32_t view,
                                     const char* key,
                                     double* out);

/*
 * Builder used by the shell to publish a snapshot. Not part of the plugin-side
 * surface. Create with a view count, set keys (view=IHS_CONFIG_GLOBAL for
 * global keys), then publish — which stamps the next generation, installs it as
 * current, and consumes the builder. Returns the published generation.
 */
typedef struct IhsConfigBuilder IhsConfigBuilder;

IHS_EXPORT IhsConfigBuilder* ihs_config_builder_new(size_t view_count);
IHS_EXPORT void ihs_config_builder_set_str(IhsConfigBuilder* builder,
                                           int32_t view,
                                           const char* key,
                                           const char* value);
IHS_EXPORT void ihs_config_builder_set_int(IhsConfigBuilder* builder,
                                           int32_t view,
                                           const char* key,
                                           int64_t value);
IHS_EXPORT void ihs_config_builder_set_bool(IhsConfigBuilder* builder,
                                            int32_t view,
                                            const char* key,
                                            int value);
IHS_EXPORT void ihs_config_builder_set_double(IhsConfigBuilder* builder,
                                              int32_t view,
                                              const char* key,
                                              double value);
/* Discard a builder without publishing. */
IHS_EXPORT void ihs_config_builder_free(IhsConfigBuilder* builder);
IHS_EXPORT uint64_t ihs_config_publish(IhsConfigBuilder* builder);

/* Config capability sub-table (IhsApi::config) — the read surface only. */
typedef struct IhsConfigApi {
  size_t struct_size;
  const IhsConfigSnapshot* (*acquire)(void);
  void (*release)(const IhsConfigSnapshot* snapshot);
  uint64_t (*generation)(const IhsConfigSnapshot* snapshot);
  size_t (*view_count)(const IhsConfigSnapshot* snapshot);
  int (*get_str)(const IhsConfigSnapshot* snapshot,
                 int32_t view,
                 const char* key,
                 char* out,
                 size_t cap,
                 size_t* needed);
  int (*get_int)(const IhsConfigSnapshot* snapshot,
                 int32_t view,
                 const char* key,
                 int64_t* out);
  int (*get_bool)(const IhsConfigSnapshot* snapshot,
                  int32_t view,
                  const char* key,
                  int* out);
  int (*get_double)(const IhsConfigSnapshot* snapshot,
                    int32_t view,
                    const char* key,
                    double* out);
} IhsConfigApi;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_CONFIG_H_ */
