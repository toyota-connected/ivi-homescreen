/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_LOCATION_H_
#define IHS_LOC_LOCATION_H_

#include <cmath>
#include <cstdint>

namespace ihs::location {

// A coordinate is usable only if finite and within the geographic range. Both
// providers validate before publishing so a malformed source (an arbitrary
// gpsd host, a bad D-Bus reply) can't push NaN/Inf or impossible coordinates to
// consumers doing projection/camera math.
inline bool ValidLatLon(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90.0 &&
         lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

// One location fix, provider-agnostic. Consumers (the map camera, a puck)
// read this; whether it came from gpsd, geoclue, or a test source is hidden
// behind ILocationProvider.
struct Position {
  double latitude = 0.0;
  double longitude = 0.0;
  double bearing_deg = 0.0;  // course over ground; valid only if has_bearing
  double speed_mps = -1.0;   // ground speed m/s; < 0 means unknown
  int mode = 0;              // gpsd-style fix mode: <2 no fix, 2 = 2D, 3 = 3D
  bool has_bearing = false;

  [[nodiscard]] bool valid() const { return mode >= 2; }
};

// A source of location fixes. Implementations (GpsdProvider, later
// GeoclueProvider) acquire asynchronously on their own thread and publish the
// latest fix; consumers poll. Kept deliberately small so it can move to a
// shared location service later without dragging the map along.
class ILocationProvider {
 public:
  virtual ~ILocationProvider() = default;

  // Begin acquiring (starts the provider's worker). Returns false if the
  // provider could not start at all; a transient source (no fix yet) still
  // returns true and reports fixes as they arrive.
  virtual bool Start() = 0;

  // Stop acquiring and join the worker. Idempotent.
  virtual void Stop() = 0;

  // Copy the most recent fix into @out. Returns false until a valid fix exists.
  virtual bool Latest(Position& out) = 0;

  // Monotonic counter bumped on each new fix, so a consumer can cheaply tell
  // whether the fix changed since it last looked (no timestamps needed).
  [[nodiscard]] virtual uint64_t generation() const = 0;
};

}  // namespace ihs::location

#endif  // IHS_LOC_LOCATION_H_
