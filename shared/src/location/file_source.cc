/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "file_source.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <utility>

#include "gpsd_provider.hpp"  // ParseTpv

namespace ihs::location {

namespace {
// Cadence assumed for a fix whose line carries no usable time (gpsd's default).
constexpr uint64_t kNominalStepNs = 1000000000ULL;  // 1 s
// Largest wall-clock wait honored in kRealtime, so a long gap in the capture (a
// paused recording) cannot stall a demo or teardown; the stamped timeline still
// reflects the true gap so a filter coasts correctly.
constexpr uint64_t kMaxRealtimeSleepNs = 5000000000ULL;  // 5 s
}  // namespace

bool ParseTpvTimeNs(const std::string& line, uint64_t& epoch_ns) {
  static constexpr char kToken[] = R"("time":")";
  const size_t at = line.find(kToken);
  if (at == std::string::npos) {
    return false;
  }
  const char* p = line.c_str() + at + (sizeof(kToken) - 1);
  int year = 0;
  int mon = 0;
  int day = 0;
  int hour = 0;
  int min = 0;
  double sec = 0.0;
  // gpsd emits ISO8601 UTC ("...Z"); read the calendar fields and the (possibly
  // fractional) seconds. The field-width caps keep a malformed capture from
  // overflowing the int conversions, and the matched-field count (== 6) is the
  // conversion-error check the bare return value would otherwise hide.
  // NOLINTNEXTLINE(cert-err34-c)
  if (std::sscanf(p, "%4d-%2d-%2dT%2d:%2d:%lf", &year, &mon, &day, &hour, &min,
                  &sec) != 6) {
    return false;
  }
  if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
      min < 0 || min > 59 || !std::isfinite(sec) || sec < 0.0 || sec >= 61.0) {
    return false;  // out-of-range fields — not a usable timestamp
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = 0;  // whole seconds folded in below to keep the fraction exact
  const time_t base = timegm(&tm);
  if (base == static_cast<time_t>(-1)) {
    return false;
  }
  // Assemble in integer nanoseconds: the whole seconds go through the integer
  // epoch and only the sub-second fraction (< 1 s, so exactly representable)
  // touches floating point. Differencing two of these stays exact.
  const auto whole_sec = static_cast<uint64_t>(sec);
  const double frac = sec - static_cast<double>(whole_sec);
  const auto frac_ns = static_cast<uint64_t>(std::llround(frac * 1e9));
  epoch_ns =
      (static_cast<uint64_t>(base) + whole_sec) * 1000000000ULL + frac_ns;
  return true;
}

FileSource::FileSource(std::string path, Pace pace, bool loop)
    : path_(std::move(path)), pace_(pace), loop_(loop) {}

FileSource::~FileSource() {
  StopWorker();
}

void FileSource::SetOnFix(FixSink on_fix) {
  on_fix_ = std::move(on_fix);
}

bool FileSource::Start() {
  {
    // Fail fast on a missing/unreadable file so a caller learns at Start(),
    // matching "returns NULL on failure" at the C ABI; the worker reopens.
    std::ifstream probe(path_);
    if (!probe) {
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) {
      return true;  // already started
    }
    running_ = true;
  }
  try {
    thread_ = std::thread([this] { Run(); });
  } catch (...) {
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false;  // roll back so the object is reusable
    return false;
  }
  return true;
}

void FileSource::Stop() {
  StopWorker();
}

void FileSource::StopWorker() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false;
  }
  cv_.notify_all();  // wake an interruptible realtime wait
  if (thread_.joinable()) {
    thread_.join();
  }
}

void FileSource::Run() {
  const uint64_t base_mono = MonotonicNs();
  bool have_prev = false;   // any fix emitted yet (persists across loop passes)
  uint64_t prev_stamp = 0;  // last stamped time, kept non-decreasing

  do {
    std::ifstream in(path_);
    if (!in) {
      break;  // file vanished between Start() and here
    }
    bool pass_have_epoch =
        false;                   // reset each pass: no epoch continuity across
    uint64_t prev_epoch_ns = 0;  // a loop boundary or a reopen
    size_t pass_fixes = 0;       // fixes emitted this pass
    std::string line;
    while (std::getline(in, line)) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) {
          return;
        }
      }
      Position p;
      if (!ParseTpv(line, p)) {
        continue;  // non-TPV or malformed
      }
      uint64_t epoch_ns = 0;
      const bool have_time = ParseTpvTimeNs(line, epoch_ns);

      // Advance the synthesized monotonic timeline. First fix ever anchors at
      // base_mono; within a pass a timed fix advances by its recorded delta
      // (clamped non-negative); otherwise (first fix of a looped pass, or a
      // line with no time) it advances by the nominal cadence.
      uint64_t dt_ns = 0;
      uint64_t stamp = base_mono;
      if (!have_prev) {
        stamp = base_mono;
      } else if (have_time && pass_have_epoch) {
        dt_ns = epoch_ns > prev_epoch_ns ? epoch_ns - prev_epoch_ns : 0;
        stamp = prev_stamp + dt_ns;
      } else {
        dt_ns = kNominalStepNs;
        stamp = prev_stamp + dt_ns;
      }
      if (have_time) {
        prev_epoch_ns = epoch_ns;
        pass_have_epoch = true;
      }
      have_prev = true;
      prev_stamp = stamp;
      p.t_monotonic_ns = stamp;

      // Realtime pacing: wait the recorded gap before emitting, interruptibly.
      if (pace_ == Pace::kRealtime && dt_ns > 0) {
        const uint64_t sleep_ns =
            dt_ns > kMaxRealtimeSleepNs ? kMaxRealtimeSleepNs : dt_ns;
        std::unique_lock<std::mutex> lk(mu_);
        if (cv_.wait_for(lk, std::chrono::nanoseconds(sleep_ns),
                         [this] { return !running_; })) {
          return;  // Stop() requested during the wait
        }
      }

      ++pass_fixes;
      if (on_fix_) {
        on_fix_(p);  // event push on the worker thread — the wake point
      }
    }

    // Stop looping if this pass yielded nothing: a capture with no usable fix
    // will not gain one on a re-read, and re-reading it in a tight loop would
    // just spin the CPU.
    if (pass_fixes == 0) {
      break;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) {
      return;
    }
  } while (loop_);
}

}  // namespace ihs::location
