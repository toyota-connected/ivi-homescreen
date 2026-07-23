/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "geoclue_provider.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "sd_bus_dynamic.hpp"

namespace ihs::location {

namespace {

constexpr char kGeoclue[] = "org.freedesktop.GeoClue2";
constexpr char kManagerPath[] = "/org/freedesktop/GeoClue2/Manager";
constexpr char kManagerIface[] = "org.freedesktop.GeoClue2.Manager";
constexpr char kClientIface[] = "org.freedesktop.GeoClue2.Client";
constexpr char kLocationIface[] = "org.freedesktop.GeoClue2.Location";

// GClueAccuracyLevel: 0 none, 1 country, 4 city, 5 neighborhood, 6 street,
// 8 exact. Request street-level; geoclue clamps to what it can provide.
constexpr uint32_t kAccuracyStreet = 6;

double ReadDouble(const SdBusApi* sd,
                  sd_bus* bus,
                  const char* path,
                  const char* member) {
  sd_bus_error err{};
  double v = -1.0;
  if (sd->get_property_trivial(bus, kGeoclue, path, kLocationIface, member,
                               &err, 'd', &v) < 0) {
    v = -1.0;
  }
  sd->error_free(&err);
  return v;
}

// sd-bus signal handler for Client.LocationUpdated(old, new); reads the new
// Location object path and hands it to the provider.
int LocationUpdatedCb(sd_bus_message* m, void* userdata, sd_bus_error* /*e*/) {
  const SdBusApi* sd = SdBusLoad();
  if (sd == nullptr) {
    return 0;
  }
  const char* old_path = nullptr;
  const char* new_path = nullptr;
  if (sd->message_read(m, "oo", &old_path, &new_path) < 0 ||
      new_path == nullptr) {
    return 0;
  }
  static_cast<GeoclueProvider*>(userdata)->OnLocationObject(new_path);
  return 0;
}

}  // namespace

GeoclueProvider::GeoclueProvider(std::string desktop_id,
                                 std::string bus_address)
    : desktop_id_(std::move(desktop_id)),
      bus_address_(std::move(bus_address)) {}

GeoclueProvider::~GeoclueProvider() {
  StopWorker();
}

bool GeoclueProvider::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return true;  // already started
  }
  try {
    thread_ = std::thread([this] { Run(); });
  } catch (...) {
    running_.store(false);  // roll back so the object is reusable / not stuck
    return false;
  }
  return true;
}

void GeoclueProvider::Stop() {
  StopWorker();
}

void GeoclueProvider::StopWorker() {
  if (!running_.exchange(false)) {
    return;  // already stopped
  }
  // The worker blocks in sd_bus_wait for at most 200 ms, then re-checks
  // running_, so the join returns promptly.
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool GeoclueProvider::Latest(Position& out) {
  const std::lock_guard<std::mutex> lock(mu_);
  if (!have_fix_) {
    return false;
  }
  out = latest_;
  return true;
}

uint64_t GeoclueProvider::generation() const {
  return generation_.load();
}

void GeoclueProvider::OnLocationObject(const char* location_path) {
  if (sd_ == nullptr) {
    return;
  }
  sd_bus_error err{};
  double lat = 0.0;
  double lon = 0.0;
  if (sd_->get_property_trivial(bus_, kGeoclue, location_path, kLocationIface,
                                "Latitude", &err, 'd', &lat) < 0 ||
      sd_->get_property_trivial(bus_, kGeoclue, location_path, kLocationIface,
                                "Longitude", &err, 'd', &lon) < 0) {
    sd_->error_free(&err);
    return;
  }
  sd_->error_free(&err);

  const double heading = ReadDouble(sd_, bus_, location_path, "Heading");
  const double speed = ReadDouble(sd_, bus_, location_path, "Speed");

  Position p;
  p.latitude = lat;
  p.longitude = lon;
  p.mode = 3;  // geoclue reports a fix (no 2D/3D distinction); treat as valid
  if (heading >= 0.0) {
    p.bearing_deg = heading;
    p.has_bearing = true;
  }
  if (speed >= 0.0) {
    p.speed_mps = speed;
  }
  {
    const std::lock_guard<std::mutex> lock(mu_);
    latest_ = p;
    have_fix_ = true;
  }
  generation_.fetch_add(1);
}

bool GeoclueProvider::Setup() {
  sd_ = SdBusLoad();
  if (sd_ == nullptr) {
    return false;  // libsystemd not present -> geoclue unavailable
  }

  int r = 0;
  if (bus_address_.empty()) {
    r = sd_->open_system(&bus_);
  } else if (bus_address_ == "user") {
    r = sd_->open_user(&bus_);
  } else {
    r = sd_->bus_new(&bus_);
    if (r >= 0) {
      r = sd_->set_address(bus_, bus_address_.c_str());
    }
    if (r >= 0) {
      sd_->set_bus_client(bus_, 1);
      r = sd_->start(bus_);
    }
  }
  if (r < 0 || bus_ == nullptr) {
    return false;
  }

  sd_bus_error err{};
  sd_bus_message* reply = nullptr;

  // Manager.GetClient -> our per-app client object path.
  r = sd_->call_method(bus_, kGeoclue, kManagerPath, kManagerIface, "GetClient",
                       &err, &reply, "");
  if (r < 0) {
    std::fprintf(stderr, "[ihs.location] geoclue: GetClient failed: %s\n",
                 err.message ? err.message : "?");
    sd_->error_free(&err);
    return false;
  }
  const char* client_path = nullptr;
  r = sd_->message_read(reply, "o", &client_path);
  if (r >= 0 && client_path != nullptr) {
    client_path_ = client_path;
  }
  reply = sd_->message_unref(reply);
  if (client_path_.empty()) {
    sd_->error_free(&err);
    return false;
  }

  // Identify + request accuracy before starting. geoclue requires both, so a
  // failure is fatal: fail Setup() and let the retry loop try again rather than
  // run a client that can never receive fixes.
  if (sd_->set_property(bus_, kGeoclue, client_path_.c_str(), kClientIface,
                        "DesktopId", &err, "s", desktop_id_.c_str()) < 0) {
    std::fprintf(stderr, "[ihs.location] geoclue: set DesktopId failed: %s\n",
                 err.message ? err.message : "?");
    sd_->error_free(&err);
    return false;
  }
  if (sd_->set_property(bus_, kGeoclue, client_path_.c_str(), kClientIface,
                        "RequestedAccuracyLevel", &err, "u",
                        kAccuracyStreet) < 0) {
    std::fprintf(
        stderr,
        "[ihs.location] geoclue: set RequestedAccuracyLevel failed: %s\n",
        err.message ? err.message : "?");
    sd_->error_free(&err);
    return false;
  }

  // Fixes arrive via LocationUpdated(old, new).
  r = sd_->match_signal(bus_, nullptr, kGeoclue, client_path_.c_str(),
                        kClientIface, "LocationUpdated", &LocationUpdatedCb,
                        this);
  if (r < 0) {
    sd_->error_free(&err);
    return false;
  }

  r = sd_->call_method(bus_, kGeoclue, client_path_.c_str(), kClientIface,
                       "Start", &err, &reply, "");
  if (r < 0) {
    std::fprintf(stderr, "[ihs.location] geoclue: Start failed: %s\n",
                 err.message ? err.message : "?");
    sd_->error_free(&err);
    return false;
  }
  sd_->message_unref(reply);
  sd_->error_free(&err);
  std::fprintf(stderr, "[ihs.location] geoclue: watching %s\n",
               client_path_.c_str());
  return true;
}

void GeoclueProvider::Teardown() {
  if (sd_ == nullptr || bus_ == nullptr) {
    return;
  }
  if (!client_path_.empty()) {
    sd_bus_error err{};
    sd_bus_message* reply = nullptr;
    sd_->call_method(bus_, kGeoclue, client_path_.c_str(), kClientIface, "Stop",
                     &err, &reply, "");
    sd_->message_unref(reply);
    sd_->error_free(&err);
    client_path_.clear();
  }
  bus_ = sd_->flush_close_unref(bus_);
}

void GeoclueProvider::Run() {
  while (running_.load()) {
    if (!Setup()) {
      Teardown();
      if (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));  // retry
      }
      continue;
    }
    while (running_.load()) {
      const int r = sd_->process(bus_, nullptr);
      if (r < 0) {
        break;  // bus error -> reconnect
      }
      if (r > 0) {
        continue;  // more queued, process again before waiting
      }
      sd_->wait(bus_, 200000);  // 200 ms, so running_ is re-checked
    }
    Teardown();
    if (running_.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }
}

}  // namespace ihs::location
