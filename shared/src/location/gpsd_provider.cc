/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "gpsd_provider.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace ihs::location {

namespace {

// Extract the number following "key": in a compact gpsd JSON line. Returns
// false if the key is absent or the value is not numeric. gpsd keys are
// distinct tokens, so matching the quoted "key": literal avoids false hits.
bool FindNumber(const std::string& s, const char* key, double& out) {
  const std::string token = std::string("\"") + key + "\":";
  const size_t at = s.find(token);
  if (at == std::string::npos) {
    return false;
  }
  const char* start = s.c_str() + at + token.size();
  char* end = nullptr;
  const double v = std::strtod(start, &end);
  if (end == start) {
    return false;
  }
  out = v;
  return true;
}

constexpr char kWatchCommand[] = "?WATCH={\"enable\":true,\"json\":true}\r\n";

}  // namespace

bool ParseTpv(const std::string& line, Position& out) {
  if (line.find(R"("class":"TPV")") == std::string::npos) {
    return false;
  }
  double lat = 0.0;
  double lon = 0.0;
  if (!FindNumber(line, "lat", lat) || !FindNumber(line, "lon", lon)) {
    return false;  // TPV without a position (e.g. mode 1 acquiring)
  }
  // A caller may point gpsd at an arbitrary host, so reject NaN/Inf and
  // impossible coordinates rather than propagate them downstream.
  if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0 ||
      lon < -180.0 || lon > 180.0) {
    return false;
  }
  Position p;
  p.latitude = lat;
  p.longitude = lon;
  double mode = 0.0;
  // A TPV that carries lat/lon but omits mode (or reports a non-finite mode)
  // still represents a fix; default to 3D so it counts as valid().
  p.mode = (FindNumber(line, "mode", mode) && std::isfinite(mode))
               ? static_cast<int>(mode)
               : 3;
  if (!p.valid()) {
    return false;
  }
  double track = 0.0;
  if (FindNumber(line, "track", track) && std::isfinite(track)) {
    p.bearing_deg = track;
    p.has_bearing = true;
  }
  double speed = 0.0;
  if (FindNumber(line, "speed", speed) && std::isfinite(speed) &&
      speed >= 0.0) {
    p.speed_mps = speed;
  }
  out = p;
  return true;
}

GpsdProvider::GpsdProvider(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

GpsdProvider::~GpsdProvider() {
  StopWorker();
}

bool GpsdProvider::Start() {
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

void GpsdProvider::Stop() {
  StopWorker();
}

void GpsdProvider::StopWorker() {
  if (!running_.exchange(false)) {
    return;  // already stopped
  }
  // Unblock a read() parked on the socket by shutting it down.
  const int fd = sock_.load();
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool GpsdProvider::Latest(Position& out) {
  const std::lock_guard<std::mutex> lock(mu_);
  if (!have_fix_) {
    return false;
  }
  out = latest_;
  return true;
}

uint64_t GpsdProvider::generation() const {
  return generation_.load();
}

void GpsdProvider::Run() {
  while (running_.load()) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      // gpsd may not be up yet; retry until Stop().
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    sock_.store(fd);
    ::send(fd, kWatchCommand, sizeof(kWatchCommand) - 1, MSG_NOSIGNAL);

    // Read the byte stream and split into lines; gpsd delimits reports with \n.
    std::string buf;
    char chunk[2048];
    while (running_.load()) {
      const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        break;  // gpsd closed or shutdown() from Stop()
      }
      buf.append(chunk, static_cast<size_t>(n));
      // Defensive: a gpsd report is a short single line. If a broken/hostile
      // server streams without a newline, drop the backlog so buf can't grow
      // without bound (resync on the next newline).
      if (buf.size() > 65536 && buf.find('\n') == std::string::npos) {
        buf.clear();
      }
      size_t nl = 0;
      while ((nl = buf.find('\n')) != std::string::npos) {
        const std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        Position p;
        if (ParseTpv(line, p)) {
          {
            const std::lock_guard<std::mutex> lock(mu_);
            latest_ = p;
            have_fix_ = true;
          }
          generation_.fetch_add(1);
        }
      }
    }

    sock_.store(-1);
    ::close(fd);
    if (running_.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));  // reconnect
    }
  }
}

}  // namespace ihs::location
