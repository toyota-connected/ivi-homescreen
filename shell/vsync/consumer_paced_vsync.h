/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SHELL_VSYNC_CONSUMER_PACED_VSYNC_H_
#define SHELL_VSYNC_CONSUMER_PACED_VSYNC_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include <shell/platform/embedder/embedder.h>

#include "asio/steady_timer.hpp"

#include "vsync/ivsync_provider.h"

class TaskRunner;

namespace ivi {

// Drives Flutter's vsync from DOWNSTREAM CONSUMER COMPLETION instead of a wall
// clock. The engine is handed the next vsync baton only when the consumer
// signals a free frame slot (a "credit"), so an engine that renders into a
// bounded pipeline (a swap chain / encode ring) naturally slows to the
// consumer's real drain rate -- true backpressure -- and stops entirely when
// the consumer stalls, instead of rendering frames that are dropped.
//
// A CREDIT is one free slot in the producer's pipeline. Delivering a baton
// commits a slot (the frame it triggers will fill one), so a credit is consumed
// per delivered baton and returned when the consumer releases a slot. Start
// with `pipeline_depth` credits (all slots free); in-flight frames never exceed
// it.
//
// An optional MIN PERIOD caps the delivery rate so a fast or idle consumer
// (credits always available) cannot free-run the engine faster than the target
// frame rate. min_period_ns == 0 is pure backpressure (no ceiling).
//
// Backend-agnostic: it only needs an IVsyncProvider (the baton machinery) and a
// TaskRunner (whose strand every OnVsync must marshal onto). The headless-EGL
// GLES encoder drives it today; a Vulkan encoder can drive the same object
// unchanged -- credits are a pipeline concept, not a renderer one.
//
// Threading: SubmitBaton()/AddCredit() may be called from any thread; each hops
// to the runner's strand, where all scheduling state lives (no locks). Delivery
// (FlutterEngineOnVsync) therefore always happens on the platform runner, which
// Flutter requires.
class ConsumerPacedVsyncSource {
 public:
  // `provider` is the baton machinery (owned by the backend, outlives this).
  // `min_period_ns` is the rate ceiling (0 = none). `pipeline_depth` is the
  // number of pipeline slots (the initial and maximum credit count).
  ConsumerPacedVsyncSource(IVsyncProvider& provider,
                           uint32_t min_period_ns,
                           uint32_t pipeline_depth);
  ~ConsumerPacedVsyncSource();

  ConsumerPacedVsyncSource(const ConsumerPacedVsyncSource&) = delete;
  ConsumerPacedVsyncSource& operator=(const ConsumerPacedVsyncSource&) = delete;

  // Wire the engine + platform runner and begin pacing. The runner must expose
  // a strand + io_context (TaskRunner::GetStrandContext/GetIoContext).
  void Start(FLUTTER_API_SYMBOL(FlutterEngine) engine, TaskRunner* runner);

  // The engine's vsync_callback trampoline hands the parked baton here.
  void SubmitBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  // The consumer finished with one frame slot -> return one credit. Safe from
  // any thread (a synchronous encoder's call site or an async release cb).
  void AddCredit();

  // Stop pacing, cancel the ceiling timer, and drop any parked baton.
  void Stop();

 private:
  void TryDeliver();             // strand only
  void ArmCeiling(uint64_t ns);  // strand only

  IVsyncProvider& provider_;
  const uint32_t min_period_ns_;
  const uint32_t pipeline_depth_;

  TaskRunner* runner_{nullptr};
  std::unique_ptr<asio::steady_timer> timer_;
  std::atomic<bool> running_{false};

  // Strand-only scheduling state (no lock: every mutator runs on the strand).
  int credits_{0};
  uint64_t last_deliver_ns_{0};
  bool ceiling_armed_{false};
};

}  // namespace ivi

#endif  // SHELL_VSYNC_CONSUMER_PACED_VSYNC_H_
