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
 * ihs_location: a shared location service fronting gpsd and geoclue.
 *
 * A single acquisition service any consumer (a map's puck, a compass widget, a
 * navigation plugin, Dart via FFI) can poll for the device's position, so the
 * providers live once in this .so rather than being re-implemented per plugin.
 * The service runs its backend(s) on their own worker threads; consumers poll
 * ihs_location_latest() and use ihs_location_generation() to notice new fixes.
 */

#ifndef IHS_LOCATION_H_
#define IHS_LOCATION_H_

#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One location fix. */
typedef struct IhsPosition {
  double latitude;
  double longitude;
  double bearing_deg;  /* course over ground, degrees; valid iff has_bearing */
  double speed_mps;    /* ground speed, meters/second; < 0 means unknown */
  int32_t mode;        /* gpsd-style fix mode: <2 no fix, 2 = 2D, 3 = 3D */
  int32_t has_bearing; /* 0/1 */
} IhsPosition;

/* Which backend(s) the service acquires from. */
typedef enum IhsLocationSource {
  IHS_LOCATION_GPSD = 0,    /* gpsd JSON socket */
  IHS_LOCATION_GEOCLUE = 1, /* geoclue over D-Bus */
  IHS_LOCATION_AUTO = 2,    /* gpsd primary, geoclue fallback */
} IhsLocationSource;

/* Opaque handle to a running location service. */
typedef struct IhsLocationService IhsLocationService;

/*
 * Start acquiring from @source. @config is an optional backend hint (may be
 * NULL): for gpsd, a numeric IPv4 "host:port" (default 127.0.0.1:2947 — host
 * names are not resolved); for geoclue, a D-Bus address, or the literal "user"
 * to use the session bus (default the system bus); ignored for
 * IHS_LOCATION_AUTO. Returns NULL on failure — a backend with no fix yet (or
 * that is not present) still returns a handle and simply reports no fix until
 * one arrives.
 */
IHS_EXPORT IhsLocationService* ihs_location_start(IhsLocationSource source,
                                                  const char* config);

/*
 * Copy the most recent fix into @out. Returns 1 if a valid fix exists, else 0
 * (in which case @out is left unchanged).
 */
IHS_EXPORT int ihs_location_latest(IhsLocationService* service,
                                   IhsPosition* out);

/*
 * A monotonic counter bumped on each new fix, so a consumer can tell whether
 * the fix changed since it last looked without comparing timestamps. 0 before
 * any fix (and for a NULL service).
 */
IHS_EXPORT uint64_t ihs_location_generation(IhsLocationService* service);

/* Stop acquiring and free @service; the handle is invalid afterwards. */
IHS_EXPORT void ihs_location_stop(IhsLocationService* service);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_LOCATION_H_ */
