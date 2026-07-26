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

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One location fix.
 *
 * ABI: fields are only ever APPENDED, never reordered or removed. Older callers
 * pass a shorter struct, so the two accessors differ by what they may write:
 *   - ihs_location_latest() fills only the original six fields, so it is safe
 *     for a caller built against any past version of this header.
 *   - ihs_location_latest2(out, out_size) fills up to out_size bytes, so a
 *     caller passing sizeof(IhsPosition) gets the newer fields too.
 * A caller that wants the timestamp or accuracy MUST use ihs_location_latest2;
 * ihs_location_latest leaves those fields untouched.
 */
typedef struct IhsPosition {
  double latitude;
  double longitude;
  double bearing_deg;  /* course over ground, degrees; valid iff has_bearing */
  double speed_mps;    /* ground speed, meters/second; < 0 means unknown */
  int32_t mode;        /* gpsd-style fix mode: <2 no fix, 2 = 2D, 3 = 3D */
  int32_t has_bearing; /* 0/1 */

  /* --- appended; only ihs_location_latest2 writes these --- */
  uint64_t t_monotonic_ns; /* fix arrival on CLOCK_MONOTONIC; 0 if unstamped */
  double sigma_e_m;        /* 1-sigma east position error, m; < 0 unknown */
  double sigma_n_m;        /* 1-sigma north position error, m; < 0 unknown */
  double sigma_v_mps;      /* 1-sigma speed error, m/s; < 0 unknown */
} IhsPosition;

/* Which backend(s) the service acquires from. */
typedef enum IhsLocationSource {
  IHS_LOCATION_GPSD = 0,    /* gpsd JSON socket */
  IHS_LOCATION_GEOCLUE = 1, /* geoclue over D-Bus */
  IHS_LOCATION_AUTO = 2,    /* gpsd primary, geoclue fallback */
  IHS_LOCATION_FILE = 3, /* replay a captured gpsd JSON file (@config=path) */
} IhsLocationSource;
/* @config for IHS_LOCATION_FILE is a captured gpsd JSON path, optionally
 * followed by "?" and one or more replay flags separated by ',' or '&':
 * fast (ignore the recorded cadence), realtime (default), loop (restart at
 * EOF). Unrecognized flags are ignored. E.g. "drive.jsonl?fast,loop". */

/* Opaque handle to a running location service. */
typedef struct IhsLocationService IhsLocationService;

/*
 * Start acquiring from @source. @config is an optional backend hint (may be
 * NULL): for gpsd, a numeric IPv4 "host:port" (default 127.0.0.1:2947 — host
 * names are not resolved); for geoclue, a D-Bus address, or the literal "user"
 * to use the session bus (default the system bus); for IHS_LOCATION_FILE, a
 * captured gpsd JSON path (as `gpspipe -w` writes), optionally followed by the
 * "?"-prefixed replay flags (fast / realtime / loop, comma- or &-separated)
 * described at the enum, replayed at the recorded cadence by default; ignored
 * for IHS_LOCATION_AUTO. Returns NULL on failure — a
 * backend with no fix yet (or that is not present) still returns a handle and
 * simply reports no fix until one arrives.
 *
 * Equivalent to ihs_location_start_filtered(source, config, NULL, NULL): the
 * fix is reported as received (no smoothing).
 */
IHS_EXPORT IhsLocationService* ihs_location_start(IhsLocationSource source,
                                                  const char* config);

/*
 * Start acquiring from @source (see ihs_location_start) but fuse the fixes
 * through a registered filter before reporting them. @filter_key selects the
 * filter — "kalman.cv" is the built-in constant-velocity Kalman filter, which
 * smooths a noisy source, interpolates a fresh position between fixes when
 * polled at UI rate, and coasts through a brief outage; NULL or "" means no
 * filter (identical to ihs_location_start). @filter_config is an optional
 * filter tuning string (may be NULL); for kalman.cv, "q=<value>" sets the
 * process-noise density.
 *
 * An unrecognized @filter_key (or a filter that fails to initialize) degrades
 * to no filter rather than failing the service, so a non-NULL return does NOT
 * guarantee the named filter is active — it only guarantees a running source.
 * Returns NULL only when the source itself cannot start (e.g. a missing file).
 */
IHS_EXPORT IhsLocationService* ihs_location_start_filtered(
    IhsLocationSource source,
    const char* config,
    const char* filter_key,
    const char* filter_config);

/*
 * Copy the most recent fix into @out, filling only the original six fields
 * (through has_bearing). Returns 1 if a valid fix exists, else 0 (in which case
 * @out is left unchanged). Use ihs_location_latest2 for the timestamp/accuracy.
 */
IHS_EXPORT int ihs_location_latest(IhsLocationService* service,
                                   IhsPosition* out);

/*
 * Copy the most recent fix into @out, writing at most @out_size bytes — pass
 * sizeof(IhsPosition) as compiled against the header you built with, and only
 * fields that fit are written. This is the forward-compatible accessor: a
 * caller built against a newer header gets the appended fields (timestamp,
 * accuracy); one built against an older header passes its smaller size and is
 * never overrun. Returns 1 if a valid fix exists, else 0.
 */
IHS_EXPORT int ihs_location_latest2(IhsLocationService* service,
                                    IhsPosition* out,
                                    size_t out_size);

/*
 * A monotonic counter bumped on each new fix, so a consumer can tell whether
 * the fix changed since it last looked without comparing timestamps. 0 before
 * any fix (and for a NULL service).
 */
IHS_EXPORT uint64_t ihs_location_generation(IhsLocationService* service);

/*
 * Called on each new fix. @pos points to a full IhsPosition valid only for the
 * duration of the call — a temporary, not a stable service-owned buffer, so
 * copy what you keep and do not retain the pointer. Reading only the fields
 * your header knows is safe (fields are append-only).
 * The callback runs on an internal acquisition thread, NOT the caller's — and
 * not necessarily the same one across fixes (IHS_LOCATION_AUTO forwards from
 * either the gpsd or the geoclue thread), so do not assume a single thread
 * identity. It must be thread-safe or hand the work to its own thread, and it
 * must not call ihs_location_stop() on this service (that would free the
 * service from within its own callback).
 */
typedef void (*IhsLocationCallback)(void* user_data, const IhsPosition* pos);

/*
 * Register @callback to fire on each new fix (push), replacing any prior
 * callback on @service; pass NULL to clear it. This complements the polling
 * accessors: subscribe FIRST, then call ihs_location_latest2() for the current
 * value — that order cannot MISS an update. A fix landing in the window between
 * the two calls may be delivered twice (once through each path); the callback
 * carries no generation, so it is not de-duplicated for you, but applying the
 * same fix is idempotent (same position), so a repeat is harmless. The callback
 * does not fire for the fix already present at registration.
 *
 * Replacing or clearing the callback is not a synchronization point: a callback
 * that has already begun on the acquisition thread may still be running (or
 * about to run) when this returns. Only ihs_location_stop() guarantees no
 * further callbacks, by joining that thread — so tear down consumer state the
 * callback touches only after stop(), not merely after clearing.
 */
IHS_EXPORT void ihs_location_set_callback(IhsLocationService* service,
                                          IhsLocationCallback callback,
                                          void* user_data);

/* Stop acquiring and free @service; the handle is invalid afterwards. */
IHS_EXPORT void ihs_location_stop(IhsLocationService* service);

/*
 * --- Measurement source / filter registry ----------------------------------
 *
 * A second, composable way to build the service, mirroring ihs_pv's factory
 * registry: sources push tagged IhsMeasurement records, a filter fuses them
 * into an IhsPosition. It exists so a non-GNSS sensor (CAN speed, an IMU) or a
 * real estimator (a Kalman filter) can be added without touching this ABI or
 * the enum providers above — a new sensor differs only by measurement `kind`.
 *
 * This surface is purely additive. The ihs_location_start() enum API is
 * unchanged, and with nothing registered the registry is simply empty; the two
 * ways to build the service do not interfere.
 */

/*
 * What an IhsMeasurement's value[] carries. POSITION/VELOCITY are the GNSS
 * staples; the rest let non-GNSS sensors feed the same filter unchanged — they
 * differ only by `kind`, never by interface.
 */
typedef enum IhsMeasKind {
  IHS_MEAS_POSITION_LLA = 0, /* lat, lon [, alt] (deg, deg, m)   — GNSS */
  IHS_MEAS_VELOCITY_NED = 1, /* v_n, v_e [, v_d] (m/s)           — GNSS/CAN */
  IHS_MEAS_SPEED = 2,        /* scalar ground speed (m/s)        — CAN wheel */
  IHS_MEAS_HEADING = 3,      /* course over ground (deg)         — GNSS track */
  IHS_MEAS_YAW_RATE = 4,     /* yaw rate (rad/s)                 — gyro/CAN */
  IHS_MEAS_ACCEL_BODY = 5,   /* a_x, a_y, a_z (m/s^2)            — IMU */
} IhsMeasKind;

/* IhsMeasurement.flags bits. */
enum {
  IHS_MEAS_FLAG_DEAD_RECKONED = 1u << 0, /* propagated, not directly observed */
};

/*
 * One tagged sensor measurement pushed by a source into the filter.
 *
 * ABI: struct_size versions it as IhsFrame does (a size_t leading field) — a
 * producer built against a newer header may append fields, and a consumer
 * copies only min(its sizeof, the producer's struct_size), never reading past
 * either object (the bounded-copy discipline). value_count says how many of
 * value[]/variance[] carry data for this `kind`.
 */
typedef struct IhsMeasurement {
  size_t struct_size;
  uint32_t kind;           /* IhsMeasKind */
  uint64_t t_monotonic_ns; /* arrival on CLOCK_MONOTONIC — see IhsPosition */
  uint32_t value_count;    /* components used in value[]/variance[] */
  double value[6];
  double variance[6]; /* per-component 1-sigma^2; < 0 = unknown, filter fills */
  uint32_t flags;     /* IHS_MEAS_FLAG_* */
} IhsMeasurement;

/*
 * A source pushes each measurement into this sink, which the manager supplies
 * to IhsLocationSourceOps.start. Called on the source's own acquisition thread;
 * @m is owned by the caller and valid only for the duration of the call.
 */
typedef void (*IhsMeasSink)(void* sink_user_data, const IhsMeasurement* m);

/*
 * Lifecycle of a measurement source. start() begins acquisition and pushes
 * measurements to @sink (with @sink_user_data) until stop(); both receive the
 * user_data passed to ihs_location_register_source. start() returns 1 on
 * success, 0 if it could not begin at all.
 */
typedef struct IhsLocationSourceOps {
  size_t struct_size;
  int (*start)(void* user_data, IhsMeasSink sink, void* sink_user_data);
  void (*stop)(void* user_data);
} IhsLocationSourceOps;

/*
 * A filter fuses measurements into a position estimate. estimate(t) predicts
 * the state forward to @t_monotonic_ns rather than returning the last fix, so a
 * consumer polling at UI rate gets a fresh position from a 1 Hz source — the
 * actual value of running a filter. create() returns an opaque instance (or
 * NULL on failure) that the other ops receive; estimate() returns 1 when @out
 * holds a valid estimate, else 0.
 */
typedef struct IhsLocationFilterOps {
  size_t struct_size;
  void* (*create)(void* user_data, const char* config);
  void (*destroy)(void* instance);
  void (*update)(void* instance, const IhsMeasurement* m);
  int (*estimate)(void* instance, uint64_t t_monotonic_ns, IhsPosition* out);
} IhsLocationFilterOps;

/*
 * Register a measurement source / filter under @key (e.g. "gnss.gpsd",
 * "kalman.cv"). @ops is copied (bounded by its struct_size) and @user_data is
 * passed back to each callback. Re-registering a key replaces the prior entry.
 * Returns 1 on success, 0 on invalid arguments: a NULL key/ops, a struct_size
 * too small to hold the mandatory callbacks, or a NULL mandatory callback — for
 * a source that is start(); for a filter, create()/update()/estimate(). The
 * optional teardown callbacks (source stop(), filter destroy()) may be NULL.
 * Thread-safe.
 */
IHS_EXPORT int ihs_location_register_source(const char* key,
                                            const IhsLocationSourceOps* ops,
                                            void* user_data);
IHS_EXPORT int ihs_location_register_filter(const char* key,
                                            const IhsLocationFilterOps* ops,
                                            void* user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_LOCATION_H_ */
