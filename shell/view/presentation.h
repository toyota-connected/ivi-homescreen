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

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// Telling a platform view's producer when its frames reached the screen.
//
// A backend notes, while it builds a frame, which submitted frame of each view
// that frame shows; hands the notes over when it commits the frame to the
// display; and reports them when the display says the frame is on screen --
// a page flip completing, or the host compositor's presentation feedback.

// How a frame reached the screen: wp_presentation_feedback.kind, and the
// IHS_PV_PRESENTED_* values of the plugin ABI.
constexpr uint32_t kPresentedVsync = 0x1;
constexpr uint32_t kPresentedHwClock = 0x2;
constexpr uint32_t kPresentedHwCompletion = 0x4;
constexpr uint32_t kPresentedZeroCopy = 0x8;

// When a frame reached the screen.
struct PresentationTime {
  // CLOCK_MONOTONIC, in nanoseconds: when the frame started to be shown.
  uint64_t ust_ns{0};
  // The output's refresh period in nanoseconds; 0 when unknown.
  uint32_t refresh_ns{0};
  // The output's vblank counter at that point; 0 when it has none.
  uint64_t msc{0};
  // kPresented* bits.
  uint32_t flags{0};
};

// A DRM fourcc and modifier.
using FormatModifierPair = std::pair<uint32_t, uint64_t>;

// Where the display's feedback about a view goes: its frames reaching the
// screen, and hints about how they could reach it more cheaply. Owned apart
// from the view, so feedback still in flight when the view is disposed has
// something safe to land on. Any thread.
class IPresentationSink {
 public:
  virtual ~IPresentationSink() = default;
  // The display is showing @p frame, a token the view handed out with it (see
  // ICompositorSurface), and every frame before it that was never reported
  // was not shown.
  virtual void OnPresented(uint64_t frame, const PresentationTime& when) = 0;
  // Layer @p layer_id could go on a plane of device @p dev, but no plane that
  // could take it scans out its format; they scan out @p formats. Empty
  // @p formats: the layer's format fits. Called every present a layer brings a
  // new frame, whether or not the answer changed.
  virtual void OnScanoutHint(
      uint32_t /*layer_id*/,
      uint64_t /*dev*/,
      const std::vector<FormatModifierPair>& /*formats*/) {}
};

// The frames a backend has built or committed, until the display shows them.
//
// A frame can also carry retire actions: work that must wait until the display
// no longer needs something the frame used -- handing a producer's buffer back
// once the frame that sampled it is on screen. They run when their commit is
// shown, overtaken by a later one that is, or discarded by the display, and
// every one runs exactly once (Reset aside).
class PresentationTracker {
 public:
  using Retire = std::function<void()>;

  // A frame is being built, and it shows @p frame of the view behind @p sink.
  // Called once per layer: a view whose layers came from different submits is
  // reported as its oldest, and as zero-copy only if every layer was scanned
  // out from the producer's own buffer. Frame 0 is untracked and ignored.
  // Compositor thread.
  void Note(const std::shared_ptr<IPresentationSink>& sink,
            const uint64_t frame,
            const bool zero_copy) {
    if (!sink || frame == 0) {
      return;
    }
    const std::lock_guard<std::mutex> lock(mu_);
    for (Entry& e : building_) {
      if (e.sink == sink) {
        e.frame = frame < e.frame ? frame : e.frame;
        e.zero_copy = e.zero_copy && zero_copy;
        return;
      }
    }
    building_.push_back({sink, frame, zero_copy});
  }

  // Once the frame being built is off screen, run @p retire. Compositor
  // thread.
  void NoteRetire(Retire retire) {
    if (!retire) {
      return;
    }
    const std::lock_guard<std::mutex> lock(mu_);
    retires_.push_back(std::move(retire));
  }

  // The frame being built will not reach the display: forget its notes. Its
  // retire actions ride on the next commit instead -- what it used may still
  // be in use by work already submitted.
  void Discard() {
    const std::lock_guard<std::mutex> lock(mu_);
    building_.clear();
  }

  // The frame being built was handed to the display under @p key, which
  // Presented names again when it is shown. An empty frame is not queued.
  void Commit(const uint64_t key) {
    std::vector<Retire> dropped;
    {
      const std::lock_guard<std::mutex> lock(mu_);
      if (building_.empty() && retires_.empty()) {
        return;
      }
      in_flight_.push_back({key, std::move(building_), std::move(retires_)});
      building_.clear();
      retires_.clear();
      // A display that stops reporting (a flip event lost to a driver bug, a
      // session paused mid-flip) must not grow this without bound. What falls
      // off was never reported, which a producer reads as not shown; its
      // retires run, the frame being long gone.
      while (in_flight_.size() > kMaxInFlight) {
        TakeRetires(in_flight_.front(), &dropped);
        in_flight_.pop_front();
      }
    }
    RunRetires(dropped);
  }

  // The frame committed under @p key is on screen. Frames committed before it
  // and not reported yet were replaced without being shown and are dropped.
  // A key never committed (a frame with nothing to report) changes nothing.
  // Any thread; the sinks are called without the lock held.
  void Presented(const uint64_t key, const PresentationTime& when) {
    Finish(key, &when, /*retire=*/true);
  }

  // Presented, but the time is an estimate taken when the frame was handed
  // over rather than the display's word that it is showing: report it now,
  // and hold its retire actions until the next frame is handed over -- one
  // frame later, when the work that used them has certainly finished.
  void PresentedEarly(const uint64_t key, const PresentationTime& when) {
    Finish(key, &when, /*retire=*/false);
  }

  // The display dropped the frame committed under @p key without showing it:
  // nothing to report, but what it used is free.
  void Discarded(const uint64_t key) { Finish(key, nullptr, /*retire=*/true); }

  // Everything committed, however long it has waited. For teardown, which
  // drops reports and retire actions rather than make them.
  void Reset() {
    const std::lock_guard<std::mutex> lock(mu_);
    building_.clear();
    retires_.clear();
    held_.clear();
    in_flight_.clear();
  }

  [[nodiscard]] size_t in_flight() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return in_flight_.size();
  }

 private:
  struct Entry {
    std::shared_ptr<IPresentationSink> sink;
    uint64_t frame;
    bool zero_copy;
  };
  struct Committed {
    uint64_t key;
    std::vector<Entry> entries;
    std::vector<Retire> retires;
  };
  static constexpr size_t kMaxInFlight = 4;

  static void TakeRetires(Committed& c, std::vector<Retire>* out) {
    for (Retire& r : c.retires) {
      out->push_back(std::move(r));
    }
    c.retires.clear();
  }

  static void RunRetires(const std::vector<Retire>& retires) {
    for (const Retire& r : retires) {
      r();
    }
  }

  // The commit under @p key is done with: report it at @p when (null: not
  // shown), and run its retires and those of every commit it overtook -- or,
  // without @p retire, hold its own for the next frame. Sinks and retires are
  // called without the lock held.
  void Finish(const uint64_t key,
              const PresentationTime* when,
              const bool retire) {
    std::vector<Entry> shown;
    std::vector<Retire> retires;
    {
      const std::lock_guard<std::mutex> lock(mu_);
      auto it = in_flight_.begin();
      while (it != in_flight_.end() && it->key != key) {
        ++it;
      }
      if (it == in_flight_.end()) {
        return;
      }
      shown = std::move(it->entries);
      // Whatever an earlier early report held is a frame older than this one.
      for (Retire& r : held_) {
        retires.push_back(std::move(r));
      }
      held_.clear();
      for (auto o = in_flight_.begin(); o != it; ++o) {
        TakeRetires(*o, &retires);
      }
      if (retire) {
        TakeRetires(*it, &retires);
      } else {
        held_ = std::move(it->retires);
      }
      in_flight_.erase(in_flight_.begin(), it + 1);
    }
    if (when != nullptr) {
      for (const Entry& e : shown) {
        PresentationTime t = *when;
        if (e.zero_copy) {
          t.flags |= kPresentedZeroCopy;
        }
        e.sink->OnPresented(e.frame, t);
      }
    }
    RunRetires(retires);
  }

  mutable std::mutex mu_;
  std::vector<Entry> building_;
  std::vector<Retire> retires_;
  // Retires of the last frame reported early, run by the next report.
  std::vector<Retire> held_;
  std::deque<Committed> in_flight_;
};

// The refresh period of a mode, in nanoseconds, from its pixel clock (kHz)
// and totals. 0 when the mode does not say.
constexpr uint32_t RefreshPeriodNs(const uint32_t clock_khz,
                                   const uint32_t htotal,
                                   const uint32_t vtotal) {
  if (clock_khz == 0 || htotal == 0 || vtotal == 0) {
    return 0;
  }
  const uint64_t ns =
      static_cast<uint64_t>(htotal) * vtotal * 1'000'000ULL / clock_khz;
  return ns > UINT32_MAX ? 0 : static_cast<uint32_t>(ns);
}
