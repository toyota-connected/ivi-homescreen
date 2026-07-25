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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include <vector>

#include "fallback_source.hpp"
#include "geoclue_provider.hpp"
#include "gpsd_provider.hpp"
#include "location.hpp"
#include "manager.hpp"

namespace {

using ihs::location::IEventSource;
using ihs::location::Manager;
using ihs::location::Position;

// gpsd from an optional "host:port" (default 127.0.0.1:2947).
std::unique_ptr<IEventSource> MakeGpsd(const char* config) {
  std::string host = "127.0.0.1";
  uint16_t port = 2947;
  if (config != nullptr && config[0] != '\0') {
    const std::string v(config);
    if (const std::string::size_type c = v.find(':'); c != std::string::npos) {
      if (c > 0) {
        host = v.substr(0, c);  // ":2947" (empty host) keeps the default host
      }
      const char* pstr = v.c_str() + c + 1;
      char* end = nullptr;
      const long p = std::strtol(pstr, &end, 10);
      // Require the whole substring to be the number (no trailing garbage) and
      // a valid port; otherwise keep the default.
      if (end != pstr && *end == '\0' && p > 0 && p < 65536) {
        port = static_cast<uint16_t>(p);
      }
    } else {
      host = v;
    }
  }
  return std::make_unique<ihs::location::GpsdProvider>(host, port);
}

// geoclue with an optional bus address (empty => system bus).
std::unique_ptr<IEventSource> MakeGeoclue(const char* config) {
  return std::make_unique<ihs::location::GeoclueProvider>(
      "ihs-location", config != nullptr ? config : "");
}

}  // namespace

// The opaque handle owns the event source and the Manager it drives: the source
// pushes each fix to Manager::PublishFix on its worker thread, and the Manager
// is the ILocationProvider the accessors below read. Declaration order matters
// for teardown — @source is declared last so it is destroyed first, joining its
// worker before the Manager it calls into is gone.
struct IhsLocationService {
  std::unique_ptr<ihs::location::ILocationProvider> provider;  // the Manager
  std::unique_ptr<ihs::location::IEventSource> source;
};

extern "C" {

IhsLocationService* ihs_location_start(IhsLocationSource source,
                                       const char* config) {
  // No exception may cross this C ABI boundary: construction and Start()
  // (allocations, std::thread) can throw, so translate any failure to a NULL
  // return.
  try {
    // Declare the Manager before the source: the source's worker calls into the
    // Manager (Manager::PublishFix), so on any early return or exception after
    // the source starts, reverse-order destruction must tear down the source
    // first (joining its worker) and only then the Manager it points at.
    auto manager = std::make_unique<Manager>(std::vector<std::string>{},
                                             std::string{}, std::string{});
    std::unique_ptr<IEventSource> src;
    switch (source) {
      case IHS_LOCATION_GEOCLUE:
        src = MakeGeoclue(config);
        break;
      case IHS_LOCATION_AUTO:
        // gpsd primary, geoclue fallback — the same composition as before, now
        // an event-driven combiner.
        src = std::make_unique<ihs::location::FallbackSource>(
            MakeGpsd(nullptr), MakeGeoclue(nullptr));
        break;
      case IHS_LOCATION_GPSD:
      default:
        src = MakeGpsd(config);
        break;
    }
    if (!src) {
      return nullptr;
    }
    Manager* const mgr = manager.get();
    // Drive the Manager atomically on each fix the source pushes (worker
    // thread). SetOnFix must precede the source's Start().
    src->SetOnFix([mgr](const Position& p) { mgr->PublishFix(p); });
    if (!src->Start()) {
      return nullptr;  // could not begin acquisition at all
    }
    auto service = std::make_unique<IhsLocationService>();
    service->provider = std::move(manager);
    service->source = std::move(src);
    return service.release();
  } catch (...) {
    return nullptr;
  }
}

namespace {

// Fill a full IhsPosition from the internal fix.
void Fill(IhsPosition& out, const ihs::location::Position& pos) {
  out.latitude = pos.latitude;
  out.longitude = pos.longitude;
  out.bearing_deg = pos.bearing_deg;
  out.speed_mps = pos.speed_mps;
  out.mode = pos.mode;
  out.has_bearing = pos.has_bearing ? 1 : 0;
  out.t_monotonic_ns = pos.t_monotonic_ns;
  out.sigma_e_m = pos.sigma_e_m;
  out.sigma_n_m = pos.sigma_n_m;
  out.sigma_v_mps = pos.sigma_v_mps;
}

// Offset one past has_bearing — the original struct's size, and the boundary
// this accessor must not write beyond so a pre-accuracy caller is never
// overrun.
constexpr size_t kLegacySize =
    offsetof(IhsPosition, has_bearing) + sizeof(int32_t);

}  // namespace

int ihs_location_latest(IhsLocationService* service, IhsPosition* out) {
  if (service == nullptr || !service->provider || out == nullptr) {
    return 0;
  }
  try {
    ihs::location::Position pos;
    if (!service->provider->Latest(pos)) {
      return 0;
    }
    // Legacy accessor: fill only the original fields, leaving the appended ones
    // untouched, since @out may be a caller's smaller pre-accuracy struct.
    out->latitude = pos.latitude;
    out->longitude = pos.longitude;
    out->bearing_deg = pos.bearing_deg;
    out->speed_mps = pos.speed_mps;
    out->mode = pos.mode;
    out->has_bearing = pos.has_bearing ? 1 : 0;
    return 1;
  } catch (...) {
    return 0;
  }
}

int ihs_location_latest2(IhsLocationService* service,
                         IhsPosition* out,
                         size_t out_size) {
  if (service == nullptr || !service->provider || out == nullptr ||
      out_size < kLegacySize) {
    return 0;  // too small to even hold a fix
  }
  try {
    ihs::location::Position pos;
    if (!service->provider->Latest(pos)) {
      return 0;
    }
    // Build the full struct, then copy only what the caller's buffer holds, so
    // a caller compiled against an older (shorter) header is never overrun.
    IhsPosition full{};
    Fill(full, pos);
    const size_t n =
        out_size < sizeof(IhsPosition) ? out_size : sizeof(IhsPosition);
    std::memcpy(out, &full, n);
    return 1;
  } catch (...) {
    return 0;
  }
}

uint64_t ihs_location_generation(IhsLocationService* service) {
  if (service == nullptr || !service->provider) {
    return 0;
  }
  try {
    return service->provider->generation();
  } catch (...) {
    return 0;
  }
}

void ihs_location_stop(IhsLocationService* service) {
  if (service == nullptr) {
    return;
  }
  // No exception may cross the C ABI; if a Stop() throws, swallow it and still
  // free the handle. Stop the source first so its worker is joined (no fix can
  // reach the Manager afterward), then the Manager.
  try {
    if (service->source) {
      service->source->Stop();
    }
    if (service->provider) {
      service->provider->Stop();
    }
  } catch (...) {
  }
  delete service;
}

}  // extern "C"
