/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_FILE_SOURCE_H_
#define IHS_LOC_FILE_SOURCE_H_

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "location.hpp"

namespace ihs::location {

// Parse the gpsd TPV "time" field (ISO8601 UTC, e.g. "2026-07-20T18:03:11.5Z")
// into Unix epoch nanoseconds, fractional seconds included. Returns false if
// the field is absent or malformed. Nanoseconds (not seconds-as-double) so that
// a sub-second inter-sample delta stays exact: the absolute epoch is ~1.7e9 s,
// where a double's precision is only ~0.5 us, and differencing two of them
// would corrupt a 10 ms gap. Split out (like ParseTpv) so it can be unit-tested
// and reused by the file replay source; gpsd always emits UTC with a trailing
// Z, so the zone is fixed.
bool ParseTpvTimeNs(const std::string& line, uint64_t& epoch_ns);

// Replays a captured gpsd JSON stream as an event source. The capture is
// newline-delimited, one report per line — exactly what `gpspipe -w` writes, or
// a raw dump of the gpsd socket. Each TPV line is parsed with ParseTpv and
// pushed to the sink on a worker thread, so it drives the Manager identically
// to a live gpsd, but deterministically, off a file, with no hardware. Non-TPV
// and malformed lines are skipped.
//
// Timeline: each fix is stamped (t_monotonic_ns) from a monotonic timeline
// synthesized from the recorded "time" deltas, NOT wall-clock arrival. A filter
// downstream then sees the true inter-sample dt regardless of how fast the file
// is replayed — the identical input in either pace. A line with no usable time
// advances the timeline by a nominal 1 Hz step, and a capture whose clock steps
// backward is clamped to a non-decreasing timeline.
//
// Pace: kAsFast emits back-to-back with no delay (deterministic tests / CI);
// kRealtime additionally waits the recorded inter-sample gap before each fix,
// reproducing the original cadence for a demo (the wait is interruptible by
// Stop() and capped so a gap in the capture cannot hang teardown). With @loop
// the worker restarts from the top at EOF (continuous demo); otherwise it ends
// after the last fix, leaving Latest() reporting it. SetOnFix must precede
// Start(), the same contract the gpsd/geoclue sources honor.
class FileSource : public IEventSource {
 public:
  enum class Pace { kAsFast, kRealtime };

  explicit FileSource(std::string path,
                      Pace pace = Pace::kAsFast,
                      bool loop = false);
  ~FileSource() override;

  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  void SetOnFix(FixSink on_fix) override;
  bool Start() override;  // false if the file cannot be opened
  void Stop() override;

 private:
  void Run();         // worker: read the file, stamp, pace, push
  void StopWorker();  // non-virtual; Stop() and the destructor both call it

  const std::string path_;
  const Pace pace_;
  const bool loop_;

  FixSink on_fix_;  // set before Start(); read on the worker thread

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool running_ =
      false;  // guarded by mu_; also the interruptible-wait predicate
};

}  // namespace ihs::location

#endif  // IHS_LOC_FILE_SOURCE_H_
