/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Unit tests for the gnss.file replay source: the ISO8601 time extraction and
// the event-driven replay (a synthesized monotonic timeline from the recorded
// cadence, non-TPV/malformed lines skipped, missing file rejected). The replay
// waits on a condition variable fed by the sink — no polling.

#include "file_source.hpp"

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

#include "location.hpp"

namespace {

using ihs::location::FileSource;
using ihs::location::ParseTpvTimeNs;
using ihs::location::Position;

int g_tests = 0;
int g_failures = 0;
void Check(bool cond, const char* what) {
  ++g_tests;
  if (!cond) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

std::filesystem::path WriteFixture(const std::string& name,
                                   const std::string& content) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / name;
  std::ofstream out(path, std::ios::trunc);
  out << content;
  out.close();
  return path;
}

// Collects fixes and lets a test block until a target count arrives — the
// event-driven counterpart of polling for completion.
struct Collector {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Position> fixes;

  void OnFix(const Position& p) {
    std::lock_guard<std::mutex> lk(mu);
    fixes.push_back(p);
    cv.notify_all();
  }
  bool WaitFor(size_t n, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, timeout, [&] { return fixes.size() >= n; });
  }
};

void TestTimeParse() {
  uint64_t a = 0;
  uint64_t b = 0;
  Check(
      ParseTpvTimeNs(R"({"class":"TPV","time":"2026-07-20T18:03:11.000Z"})", a),
      "parses a TPV time");
  Check(ParseTpvTimeNs(R"({"time":"2026-07-20T18:03:12.500Z"})", b),
        "parses a fractional TPV time");
  // Assert the delta (1.5 s), not an absolute epoch, so the test does not
  // depend on a hardcoded timegm result — and it stays exact in integer
  // nanoseconds.
  Check(b - a == 1500000000ULL, "time delta is 1.5 s");

  uint64_t dummy = 0;
  Check(!ParseTpvTimeNs(R"({"class":"TPV","lat":1.0})", dummy),
        "absent time returns false");
  Check(!ParseTpvTimeNs(R"({"time":"not-a-timestamp"})", dummy),
        "malformed time returns false");
}

void TestReplayTimeline() {
  // Three usable fixes at t=11, 12, 14 s; interleaved with non-TPV, garbage,
  // and a mode-1 (no-fix) TPV that must all be skipped.
  const std::string fixture =
      R"({"class":"VERSION","release":"3.22"})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:11.000Z","mode":3,"lat":48.10000,"lon":-122.2,"track":90.0,"speed":5.0,"eph":8.0})"
      "\n"
      "this is not json at all\n"
      R"({"class":"SKY","satellites":[]})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:12.000Z","mode":3,"lat":48.10009,"lon":-122.2,"speed":5.0})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:14.000Z","mode":3,"lat":48.10027,"lon":-122.2,"speed":5.0})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:15.000Z","mode":1})"
      "\n";
  const auto path = WriteFixture("ihs_file_source_timeline.jsonl", fixture);

  Collector c;
  FileSource src(path.string(), FileSource::Pace::kAsFast, /*loop=*/false);
  src.SetOnFix([&c](const Position& p) { c.OnFix(p); });
  Check(src.Start(), "Start() succeeds on an existing file");
  Check(c.WaitFor(3, std::chrono::seconds(2)), "three fixes replayed");
  src.Stop();

  std::filesystem::remove(path);

  if (c.fixes.size() < 3) {
    return;  // WaitFor already failed; avoid indexing past the end
  }
  Check(c.fixes[0].mode == 3 && std::abs(c.fixes[0].latitude - 48.10000) < 1e-6,
        "first fix carries the parsed position");
  Check(
      c.fixes[0].has_bearing && std::abs(c.fixes[0].bearing_deg - 90.0) < 1e-6,
      "first fix carries the parsed track");
  // Timeline reflects the recorded cadence (1 s then 2 s), not wall-clock
  // arrival (as-fast emits back-to-back).
  const uint64_t d1 = c.fixes[1].t_monotonic_ns - c.fixes[0].t_monotonic_ns;
  const uint64_t d2 = c.fixes[2].t_monotonic_ns - c.fixes[1].t_monotonic_ns;
  Check(d1 == 1000000000ULL, "1 s gap stamped between fix 0 and 1");
  Check(d2 == 2000000000ULL, "2 s gap stamped between fix 1 and 2");
}

void TestRealtimeDelivery() {
  // Millisecond deltas so the realtime pacing runs in ~20 ms, not seconds. This
  // exercises the interruptible-wait path and confirms the stamped timeline
  // still reflects the recorded (10 ms) cadence.
  const std::string fixture =
      R"({"class":"TPV","time":"2026-07-20T18:03:11.000Z","mode":3,"lat":48.0,"lon":-122.0})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:11.010Z","mode":3,"lat":48.0,"lon":-122.0})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:11.020Z","mode":3,"lat":48.0,"lon":-122.0})"
      "\n";
  const auto path = WriteFixture("ihs_file_source_realtime.jsonl", fixture);

  Collector c;
  FileSource src(path.string(), FileSource::Pace::kRealtime, /*loop=*/false);
  src.SetOnFix([&c](const Position& p) { c.OnFix(p); });
  Check(src.Start(), "realtime Start() succeeds");
  Check(c.WaitFor(3, std::chrono::seconds(2)),
        "three fixes replayed in realtime");
  src.Stop();
  std::filesystem::remove(path);

  if (c.fixes.size() >= 2) {
    const uint64_t d1 = c.fixes[1].t_monotonic_ns - c.fixes[0].t_monotonic_ns;
    Check(d1 == 10000000ULL, "10 ms gap stamped in realtime");
  }
}

void TestStopInterruptsRealtimeWait() {
  // A 14 s gap parks the worker in the (5 s-capped) realtime wait. Stop() must
  // interrupt it promptly rather than block for the cap.
  const std::string fixture =
      R"({"class":"TPV","time":"2026-07-20T18:03:11.000Z","mode":3,"lat":48.0,"lon":-122.0})"
      "\n"
      R"({"class":"TPV","time":"2026-07-20T18:03:25.000Z","mode":3,"lat":48.0,"lon":-122.0})"
      "\n";
  const auto path = WriteFixture("ihs_file_source_interrupt.jsonl", fixture);

  Collector c;
  FileSource src(path.string(), FileSource::Pace::kRealtime, /*loop=*/false);
  src.SetOnFix([&c](const Position& p) { c.OnFix(p); });
  src.Start();
  Check(c.WaitFor(1, std::chrono::seconds(2)), "first fix arrives immediately");

  const auto t0 = std::chrono::steady_clock::now();
  src.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  std::filesystem::remove(path);

  // Correct: Stop() returns in ~ms; a broken interrupt would wait the 5 s cap.
  Check(elapsed < std::chrono::seconds(2),
        "Stop() interrupts the realtime wait");
}

void TestMissingFile() {
  Collector c;
  FileSource src("/nonexistent/ihs/does-not-exist.jsonl");
  src.SetOnFix([&c](const Position& p) { c.OnFix(p); });
  Check(!src.Start(), "Start() fails on a missing file");
  Check(c.fixes.empty(), "no fixes from a missing file");
}

}  // namespace

int main() {
  TestTimeParse();
  TestReplayTimeline();
  TestRealtimeDelivery();
  TestStopInterruptsRealtimeWait();
  TestMissingFile();

  if (g_failures == 0) {
    std::printf("file_source_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "file_source_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
