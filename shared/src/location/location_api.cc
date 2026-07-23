/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// C-ABI front (ihs_location_*) over the internal C++ providers. See
// include/ihs/location.h for the contract.

#include "ihs/location.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "fallback_location_provider.hpp"
#include "geoclue_provider.hpp"
#include "gpsd_provider.hpp"
#include "location.hpp"

// The opaque handle simply owns a running provider.
struct IhsLocationService {
  std::unique_ptr<ihs::location::ILocationProvider> provider;
};

namespace {

// gpsd from an optional "host:port" (default 127.0.0.1:2947).
std::unique_ptr<ihs::location::ILocationProvider> MakeGpsd(const char* config) {
  std::string host = "127.0.0.1";
  uint16_t port = 2947;
  if (config != nullptr && config[0] != '\0') {
    const std::string v(config);
    if (const std::string::size_type c = v.find(':'); c != std::string::npos) {
      host = v.substr(0, c);
      char* end = nullptr;
      const long p = std::strtol(v.c_str() + c + 1, &end, 10);
      if (p > 0 && p < 65536) {
        port = static_cast<uint16_t>(p);
      }
    } else {
      host = v;
    }
  }
  return std::make_unique<ihs::location::GpsdProvider>(host, port);
}

// geoclue with an optional bus address (empty => system bus).
std::unique_ptr<ihs::location::ILocationProvider> MakeGeoclue(
    const char* config) {
  return std::make_unique<ihs::location::GeoclueProvider>(
      "ihs-location", config != nullptr ? config : "");
}

}  // namespace

extern "C" {

IhsLocationService* ihs_location_start(IhsLocationSource source,
                                       const char* config) {
  // No exception may cross this C ABI boundary: provider construction and
  // Start() (allocations, std::thread) can throw, so translate any failure to
  // a NULL return.
  try {
    std::unique_ptr<ihs::location::ILocationProvider> provider;
    switch (source) {
      case IHS_LOCATION_GEOCLUE:
        provider = MakeGeoclue(config);
        break;
      case IHS_LOCATION_AUTO:
        provider = std::make_unique<ihs::location::FallbackLocationProvider>(
            MakeGpsd(nullptr), MakeGeoclue(nullptr));
        break;
      case IHS_LOCATION_GPSD:
      default:
        provider = MakeGpsd(config);
        break;
    }
    if (!provider) {
      return nullptr;
    }
    auto service = std::make_unique<IhsLocationService>();
    service->provider = std::move(provider);
    if (!service->provider->Start()) {
      return nullptr;  // could not start at all — don't hand back a dead handle
    }
    return service.release();
  } catch (...) {
    return nullptr;
  }
}

int ihs_location_latest(IhsLocationService* service, IhsPosition* out) {
  if (service == nullptr || !service->provider || out == nullptr) {
    return 0;
  }
  ihs::location::Position pos;
  if (!service->provider->Latest(pos)) {
    return 0;
  }
  out->latitude = pos.latitude;
  out->longitude = pos.longitude;
  out->bearing_deg = pos.bearing_deg;
  out->speed_mps = pos.speed_mps;
  out->mode = pos.mode;
  out->has_bearing = pos.has_bearing ? 1 : 0;
  return 1;
}

uint64_t ihs_location_generation(IhsLocationService* service) {
  return (service != nullptr && service->provider)
             ? service->provider->generation()
             : 0;
}

void ihs_location_stop(IhsLocationService* service) {
  if (service == nullptr) {
    return;
  }
  if (service->provider) {
    service->provider->Stop();
  }
  delete service;
}

}  // extern "C"
