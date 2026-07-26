/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// End-to-end test of the ihs_location_start_filtered() wiring: a gnss.file
// source replaying a captured gpsd stream, fused through the kalman.cv filter,
// delivered to a consumer via the push callback. Exercises the real C ABI
// (location_api.cc) over the whole location subsystem, not the pieces in
// isolation. Fixes are collected through a condition variable fed by the
// callback — no polling.

#include "ihs/location.h"

#include <unistd.h>  // getpid

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

int g_tests = 0;
int g_failures = 0;
void Check(bool cond, const char* what) {
  ++g_tests;
  if (!cond) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

constexpr double kLat0 = 48.0;
constexpr double kLon0 = -122.0;
constexpr double kMetersPerDegLat = 111320.0;
double MetersPerDegLon(double lat) {
  return kMetersPerDegLat * std::cos(lat * 3.14159265358979323846 / 180.0);
}

// A clean eastward track as a captured gpsd JSON stream: @n fixes, @dt_s apart,
// moving east at @v_e m/s from the origin. No noise — this checks that the wire
// delivers filtered fixes that track the truth, not the filter's noise math
// (covered by kalman_cv_test / filter_test).
std::filesystem::path WriteTrackFixture(const std::string& name,
                                        double v_e,
                                        int n,
                                        double dt_s) {
  // Per-process filename so concurrent runs (ctest -j, a shared runner) cannot
  // truncate each other's fixtures.
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     (std::to_string(getpid()) + "-" + name);
  std::ofstream out(path, std::ios::trunc);
  Check(out.is_open(), "fixture temp file opened for write");
  const double m_per_deg_lon = MetersPerDegLon(kLat0);
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) * dt_s;
    const double lon = kLon0 + (v_e * t) / m_per_deg_lon;
    char line[256];
    std::snprintf(line, sizeof(line),
                  R"({"class":"TPV","time":"2026-07-20T18:03:%06.3fZ",)"
                  R"("mode":3,"lat":%.8f,"lon":%.8f,"speed":%.3f,"eph":5.0})",
                  11.0 + t, kLat0, lon, v_e);
    out << line << "\n";
  }
  out.close();
  return path;
}

// Collects pushed fixes; a test blocks until enough arrive (event-driven).
struct Collector {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<IhsPosition> fixes;

  static void Thunk(void* user_data, const IhsPosition* p) {
    auto* self = static_cast<Collector*>(user_data);
    std::lock_guard<std::mutex> lk(self->mu);
    self->fixes.push_back(*p);
    self->cv.notify_all();
  }
  bool WaitFor(size_t n, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, timeout, [&] { return fixes.size() >= n; });
  }
  size_t count() {
    std::lock_guard<std::mutex> lk(mu);
    return fixes.size();
  }
  IhsPosition last() {
    std::lock_guard<std::mutex> lk(mu);
    return fixes.back();
  }
};

void TestFileThroughKalman() {
  const double v_e = 20.0;
  const int n = 40;
  const double dt_s = 0.025;  // 40 * 25 ms = 1 s of realtime replay
  const auto path = WriteTrackFixture("ihs_wire_kalman.jsonl", v_e, n, dt_s);

  Collector c;
  IhsLocationService* svc = ihs_location_start_filtered(
      IHS_LOCATION_FILE, path.string().c_str(), "kalman.cv", nullptr);
  Check(svc != nullptr, "start FILE + kalman.cv");
  if (svc == nullptr) {
    std::filesystem::remove(path);
    return;
  }
  ihs_location_set_callback(svc, &Collector::Thunk, &c);

  // Wait for most of the track to replay (realtime, ~1 s).
  Check(c.WaitFor(30, std::chrono::seconds(5)), "filtered fixes delivered");

  ihs_location_stop(svc);
  std::filesystem::remove(path);

  Check(c.count() >= 30, "enough fixes arrived");
  if (c.count() < 1) {
    return;
  }
  const IhsPosition p = c.last();
  // The estimate must track the (clean) truth: at the last delivered fix the
  // vehicle is near v_e * t east of the origin.
  Check(std::isfinite(p.latitude) && std::isfinite(p.longitude),
        "filtered fix is finite");
  Check(std::abs(p.latitude - kLat0) * kMetersPerDegLat < 5.0,
        "filtered fix stays on the east-west line (north error small)");
  // The filter recovers the eastward motion (speed from a position-only
  // source).
  Check(p.speed_mps > v_e * 0.5, "filter recovered the eastward speed");
  Check(p.mode == 3, "3D source keeps mode 3 through the filter");
}

void TestFileThroughCtrv() {
  // The other built-in filter is reachable the same way end to end.
  const auto path = WriteTrackFixture("ihs_wire_ctrv.jsonl", 15.0, 40, 0.025);
  Collector c;
  IhsLocationService* svc = ihs_location_start_filtered(
      IHS_LOCATION_FILE, path.string().c_str(), "kalman.ctrv", nullptr);
  Check(svc != nullptr, "start FILE + kalman.ctrv");
  if (svc == nullptr) {
    std::filesystem::remove(path);
    return;
  }
  ihs_location_set_callback(svc, &Collector::Thunk, &c);
  Check(c.WaitFor(30, std::chrono::seconds(5)), "ctrv fixes delivered");
  ihs_location_stop(svc);
  std::filesystem::remove(path);
  if (c.count() > 0) {
    const IhsPosition p = c.last();
    Check(std::isfinite(p.latitude) && std::isfinite(p.longitude) &&
              p.speed_mps > 15.0 * 0.5,
          "ctrv delivers a finite fix with recovered speed");
  }
}

void TestFilePassthrough() {
  // The same source with no filter still delivers fixes (the passthrough path).
  const auto path =
      WriteTrackFixture("ihs_wire_passthrough.jsonl", 10.0, 20, 0.02);
  Collector c;
  IhsLocationService* svc =
      ihs_location_start(IHS_LOCATION_FILE, path.string().c_str());
  Check(svc != nullptr, "start FILE (passthrough)");
  if (svc == nullptr) {
    std::filesystem::remove(path);
    return;
  }
  ihs_location_set_callback(svc, &Collector::Thunk, &c);
  Check(c.WaitFor(15, std::chrono::seconds(5)), "passthrough fixes delivered");
  ihs_location_stop(svc);
  std::filesystem::remove(path);
}

void TestFileFastPacing() {
  // 12 fixes at a 1 s recorded cadence: realtime would deliver only ~2 in 2 s,
  // but "?fast" ignores the cadence and bursts them. "loop" is added so the
  // source keeps emitting until the callback is installed — a single fast pass
  // could otherwise finish before ihs_location_set_callback() runs, making the
  // count timing-dependent. Realtime+loop would still need 12 s for 12 fixes,
  // so hitting 12 within 2 s proves the recorded cadence is ignored.
  const auto path = WriteTrackFixture("ihs_wire_fast.jsonl", 5.0, 12, 1.0);
  const std::string spec = path.string() + "?fast,loop";
  Collector c;
  IhsLocationService* svc = ihs_location_start(IHS_LOCATION_FILE, spec.c_str());
  Check(svc != nullptr, "start FILE ?fast");
  if (svc != nullptr) {
    ihs_location_set_callback(svc, &Collector::Thunk, &c);
    Check(c.WaitFor(12, std::chrono::seconds(2)),
          "?fast delivers fixes without waiting the recorded cadence");
    ihs_location_stop(svc);
  }
  std::filesystem::remove(path);
}

void TestFileLoop() {
  // A 5-fix capture with "?loop" (and "fast" so the passes fly): more than 5
  // fixes arrive, proving it restarted at EOF.
  const auto path = WriteTrackFixture("ihs_wire_loop.jsonl", 5.0, 5, 0.02);
  // '&' separator here (TestFileFastPacing uses ',') so both documented
  // separators are exercised.
  const std::string spec = path.string() + "?loop&fast";
  Collector c;
  IhsLocationService* svc = ihs_location_start(IHS_LOCATION_FILE, spec.c_str());
  Check(svc != nullptr, "start FILE ?loop");
  if (svc != nullptr) {
    ihs_location_set_callback(svc, &Collector::Thunk, &c);
    Check(c.WaitFor(15, std::chrono::seconds(3)),
          "?loop replays past EOF (more fixes than the file holds)");
    ihs_location_stop(svc);
  }
  std::filesystem::remove(path);
}

void TestBadPathAndKeys() {
  // A missing file fails the start.
  IhsLocationService* svc = ihs_location_start_filtered(
      IHS_LOCATION_FILE, "/nonexistent/ihs/nope.jsonl", "kalman.cv", nullptr);
  Check(svc == nullptr, "missing file fails start");

  // An unknown filter key degrades to the passthrough rather than failing (the
  // file still opens), so the start succeeds — and the degrade is identical to
  // the unfiltered path, preserving the source's reported speed (5 m/s) rather
  // than dropping it by decomposing to a position-only measurement.
  const double v_e = 5.0;
  const auto path = WriteTrackFixture("ihs_wire_badkey.jsonl", v_e, 20, 0.02);
  Collector c;
  IhsLocationService* svc2 = ihs_location_start_filtered(
      IHS_LOCATION_FILE, path.string().c_str(), "no.such.filter", nullptr);
  Check(svc2 != nullptr,
        "unknown filter key still starts (degrades to passthrough)");
  if (svc2 != nullptr) {
    ihs_location_set_callback(svc2, &Collector::Thunk, &c);
    Check(c.WaitFor(10, std::chrono::seconds(5)),
          "degraded path delivers fixes");
    ihs_location_stop(svc2);
    if (c.count() > 0) {
      Check(std::abs(c.last().speed_mps - v_e) < 0.5,
            "degrade-to-passthrough preserves the source speed (not dropped)");
    }
  }
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestFileThroughKalman();
  TestFileThroughCtrv();
  TestFilePassthrough();
  TestFileFastPacing();
  TestFileLoop();
  TestBadPathAndKeys();

  if (g_failures == 0) {
    std::printf("wire_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "wire_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
