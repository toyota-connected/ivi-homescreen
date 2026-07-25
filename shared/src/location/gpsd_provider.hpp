/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_GPSD_PROVIDER_H_
#define IHS_LOC_GPSD_PROVIDER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "location.hpp"

namespace ihs::location {

// Parse one line of gpsd's JSON reporting protocol. Returns true and fills @out
// only for a TPV (time-position-velocity) report that carries a usable fix
// (mode >= 2 with lat/lon); every other class (VERSION, SKY, DEVICES, ...) and
// any malformed line returns false. Split out from the socket so it can be
// unit-tested against captured gpsd output. gpsd emits compact single-line
// objects, so a targeted field scan is sufficient (no full JSON parser).
bool ParseTpv(const std::string& line, Position& out);

// ILocationProvider backed by gpsd (gpsd.io). Connects to the gpsd JSON socket
// (default 127.0.0.1:2947), issues ?WATCH, and publishes each TPV fix. Runs a
// worker thread that reconnects if gpsd drops, so it tolerates gpsd starting
// after the map. This is the primary provider; GeoclueProvider is the fallback.
class GpsdProvider : public ILocationProvider, public IEventSource {
 public:
  explicit GpsdProvider(std::string host = "127.0.0.1", uint16_t port = 2947);
  ~GpsdProvider() override;

  GpsdProvider(const GpsdProvider&) = delete;
  GpsdProvider& operator=(const GpsdProvider&) = delete;

  bool Start() override;
  void Stop() override;
  bool Latest(Position& out) override;
  [[nodiscard]] uint64_t generation() const override;

  // Event push: install a sink invoked with each new fix as it arrives, on the
  // worker thread. Must be called before Start() (the worker reads it without a
  // lock, relying on the happens-before of thread creation). This is how the
  // location Manager drives on gpsd events instead of polling generation().
  void SetOnFix(FixSink on_fix) override;

 private:
  void Run();         // worker: connect, WATCH, read TPV lines, publish
  void StopWorker();  // non-virtual; Stop() and the destructor both call it

  const std::string host_;
  const uint16_t port_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int> sock_{-1};  // current socket fd, -1 when not connected

  FixSink on_fix_;  // set before Start(); invoked on the worker thread

  mutable std::mutex mu_;
  Position latest_;
  bool have_fix_ = false;
  std::atomic<uint64_t> generation_{0};
};

}  // namespace ihs::location

#endif  // IHS_LOC_GPSD_PROVIDER_H_
