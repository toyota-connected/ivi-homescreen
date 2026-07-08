// shell/logging/dlt/worker.hpp
#pragma once

#include "compat.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace ihs::dlt {

class ContextCache;
class LibDltLoader;
class RingRegistry;

// Background drainer. Wakes periodically (or on explicit flush()) and pulls
// slots from every ThreadRing through the registry, dispatching them to
// libdlt via the loader.
class Worker {
 public:
  Worker(RingRegistry& registry,
         ContextCache& cache,
         LibDltLoader& loader) noexcept;

  ~Worker();

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void start();
  void stop();
  void flush() noexcept;

 private:
  void run(ihs::stop_token stop);
  void drain_all();

  RingRegistry& registry_;
  ContextCache& cache_;
  LibDltLoader& loader_;

  ihs::jthread thread_;
  std::mutex mu_;
  std::condition_variable wake_cv_;
  std::atomic<bool> flush_requested_{false};
  std::atomic<bool> running_{false};

  static constexpr std::chrono::milliseconds kInterval{10};
};

}  // namespace ihs::dlt
