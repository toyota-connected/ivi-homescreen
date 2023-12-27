#pragma once

#include <atomic>
#include <mutex>

#include "flutter_desktop_engine_state.h"
#include "shell/task_runner.h"

struct FlutterDesktopEngineState;

// State associated with the messenger used to communicate with the engine.
struct FlutterDesktopMessenger {
  FlutterDesktopMessenger() = default;

  /// Increments the reference count.
  ///
  /// Thread-safe.
  void AddRef() { ref_count_.fetch_add(1); }

  /// Decrements the reference count and deletes the object if the count has
  /// gone to zero.
  ///
  /// Thread-safe.
  void Release() {
    if (ref_count_.fetch_sub(1) <= 1) {
      delete this;
    }
  }

  /// Getter for the engine field.
  [[nodiscard]] FlutterDesktopEngineState* GetEngine() const { return engine_; }

  /// Setter for the engine field.
  /// Thread-safe.
  void SetEngine(FlutterDesktopEngineState* engine) {
    std::scoped_lock lock(mutex_);
    engine_ = engine;
  }

  /// Returns the mutex associated with the |FlutterDesktopMessenger|.
  ///
  /// This mutex is used to synchronize reading or writing state inside the
  /// |FlutterDesktopMessenger| (ie |engine_|).
  std::mutex& GetMutex() { return mutex_; }

  FlutterDesktopMessenger(const FlutterDesktopMessenger& value) = delete;
  FlutterDesktopMessenger& operator=(const FlutterDesktopMessenger& value) =
      delete;

private:
  // The engine that backs this messenger.
  FlutterDesktopEngineState* engine_{};
  std::atomic<int32_t> ref_count_ = 0;
  std::mutex mutex_;
};
