/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_GEOCLUE_PROVIDER_H_
#define IHS_LOC_GEOCLUE_PROVIDER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "location.hpp"
#include "sd_bus_dynamic.hpp"

namespace ihs::location {

// ILocationProvider backed by geoclue (freedesktop.org's location service) over
// D-Bus. sd-bus (libsystemd) is loaded at runtime via SdBusLoad() so ihs_shared
// carries no build- or link-time dependency on libsystemd; if the library is
// absent the provider simply reports no fix. This is the FALLBACK: geoclue
// aggregates network/Wi-Fi and, when present, GPS. GpsdProvider is the primary;
// the engine selects one.
//
// A worker thread owns the bus: it creates a geoclue Client, sets DesktopId +
// accuracy, subscribes to LocationUpdated, calls Start, and pumps the bus. Each
// location update is published as a Position. The bus address is passed to the
// constructor (empty = system bus; "user" = session bus; any other value is an
// explicit D-Bus address), so a stand-in geoclue can be tested on the session
// bus without a system-bus name.
class GeoclueProvider final : public ILocationProvider, public IEventSource {
 public:
  // @desktop_id must match geoclue's allow-list expectation (a .desktop id);
  // @bus_address empty means the system bus.
  explicit GeoclueProvider(std::string desktop_id = "ihs-location",
                           std::string bus_address = {});
  ~GeoclueProvider() override;

  GeoclueProvider(const GeoclueProvider&) = delete;
  GeoclueProvider& operator=(const GeoclueProvider&) = delete;

  bool Start() override;
  void Stop() override;
  bool Latest(Position& out) override;
  [[nodiscard]] uint64_t generation() const override;

  // Read a geoclue Location object's properties and publish a Position. Public
  // only so the file-static D-Bus signal callback (in the .cc) can reach it;
  // runs on the worker thread and is not part of the intended API.
  void OnLocationObject(const char* location_path);

  // Event push: install a sink invoked with each new fix as it arrives, on the
  // worker thread. Must be called before Start() (read without a lock, relying
  // on the happens-before of thread creation). Mirrors GpsdProvider::SetOnFix.
  void SetOnFix(FixSink on_fix) override;

 private:
  void Run();         // worker: set up the client, then pump the bus
  void StopWorker();  // non-virtual; Stop() and the destructor both call it
  bool Setup();       // connect, GetClient, configure, subscribe, Start
  void Teardown();

  const std::string desktop_id_;
  const std::string bus_address_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  const SdBusApi* sd_ = nullptr;  // resolved libsystemd entry points
  sd_bus* bus_ = nullptr;         // worker-thread only
  std::string client_path_;       // worker-thread only

  FixSink on_fix_;  // set before Start(); invoked on the worker thread

  mutable std::mutex mu_;
  Position latest_;
  bool have_fix_ = false;
  std::atomic<uint64_t> generation_{0};
};

}  // namespace ihs::location

#endif  // IHS_LOC_GEOCLUE_PROVIDER_H_
