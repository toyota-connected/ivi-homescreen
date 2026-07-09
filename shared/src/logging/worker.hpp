// shell/logging/dlt/worker.hpp
#pragma once

#include "compat.hpp"
#include "log_sink.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace ihs::dlt {

class ContextCache;
class RingRegistry;

// Background drainer. Wakes periodically (or on explicit flush()) and pulls
// slots from every ThreadRing through the registry, fanning each out to every
// active sink (DLT / console / file).
class Worker {
 public:
  Worker(RingRegistry& registry, ContextCache& cache) noexcept;

  ~Worker();

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  // sinks must outlive the worker (owned by the bridge's SinkSet).
  void start(const std::vector<std::unique_ptr<Sink>>& sinks);
  void stop();
  void flush() noexcept;

 private:
  void run(ihs::stop_token stop);
  // Drains every ring; returns the number of slots dispatched this pass.
  std::size_t drain_all();
  void flush_sinks() noexcept;

  RingRegistry& registry_;
  ContextCache& cache_;
  const std::vector<std::unique_ptr<Sink>>* sinks_ = nullptr;

  ihs::jthread thread_;
  std::mutex mu_;
  std::condition_variable wake_cv_;
  std::atomic<bool> flush_requested_{false};
  std::atomic<bool> running_{false};

  // Producers push lock-free without signalling, so the worker polls. It
  // polls tightly while there is traffic and backs off to a low-power idle
  // cadence after a quiet spell, so an ENABLE_DLT=ON build does not spin at
  // 100 Hz while nothing is logging. An explicit flush() still wakes it
  // immediately, so the backoff only affects passive draining.
  static constexpr std::chrono::milliseconds kActiveInterval{10};
  static constexpr std::chrono::milliseconds kIdleInterval{250};
  static constexpr int kEmptyPollsBeforeIdle = 20;  // ~200 ms of quiet
};

}  // namespace ihs::dlt
