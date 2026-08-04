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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace homescreen {

// Which backend family is resolving — selects which name field of an
// OutputMatch is compared against an OutputInfo::name.
enum class BackendFamily {
  kWayland,  // compare OutputMatch::wl_name        (wl_output v4 name)
  kDrm,      // compare OutputMatch::drm_connector  (KMS connector name)
};

// A live (or, on DRM, enumerated-but-disconnected) physical output. `name` is
// the stable join key: the DRM connector name ("DP-1", "HDMI-A-1") or the
// wl_output v4 name. The other fields are descriptive (EDID / current mode).
struct OutputInfo {
  std::string name;
  // Integrator-assigned role name, from the connector's IHS_OUTPUT_NAME udev
  // property. Empty unless a rule sets one. This is the only key that survives
  // a change of board: the connector name and the card number both move, and
  // many panels publish no EDID to be matched on.
  //
  // DRM only for now: DrmOutputProvider fills it, the Wayland Display does not.
  // A wl_output carries no connector identity a client can join on directly --
  // the name a compositor advertises need not be the kernel's (Mutter reports
  // HDMI-1 for HDMI-A-1, while Weston and wlroots pass it through) -- so that
  // path needs a normalization step and its own verification. Until then an
  // output_id match on Wayland finds nothing and parks, which ResolveOutput
  // reports.
  std::optional<std::string> output_id;
  std::string make;                   // EDID PNP id, e.g. "GSM"
  std::string model;                  // EDID model name
  std::optional<std::string> serial;  // EDID serial; DRM-only, often absent
  int32_t width_px = 0;
  int32_t height_px = 0;
  int32_t mm_width = 0;  // physical size from EDID
  int32_t mm_height = 0;
  double refresh_hz = 0.0;
  bool connected = false;  // DRM may enumerate a disconnected port
  uint64_t handle = 0;     // opaque: wl_output* / drm connector id
};

// Per-bundle output binding (Configuration::Config::view.output). The most
// specific field that matches a live output wins; see ResolveOutput.
struct OutputMatch {
  // Role name assigned by a udev rule (IHS_OUTPUT_NAME). The most specific
  // tier, and the only one portable across boards -- see ReadOutputIds.
  // Populated on the DRM backends only; see OutputInfo::output_id.
  std::optional<std::string> output_id;
  std::optional<std::string> wl_name;        // Wayland: wl_output name
  std::optional<std::string> drm_connector;  // DRM: kernel connector name
  std::optional<std::string> edid_serial;    // DRM-only refinement
  std::optional<uint32_t> index;             // deprecated last resort

  // Warm the engine (suspended, no scanout) at startup even if the target
  // output is absent, for instant first paint on plug-in. Costs held RAM/GPU.
  bool preload = false;

  // What to do with a running view when its output disappears.
  enum class OnDisconnect {
    kSuspend,   // keep the engine warm, stop presenting (instant replug)
    kTeardown,  // free the engine (cold-start on return)
  };
  OnDisconnect on_disconnect = OnDisconnect::kSuspend;

  // Position of this view's display in the combined pointer/desktop space, for
  // multi-display input routing. Unset = origin (0,0). Only meaningful when
  // several views share one input seat (multiple displays on one card): the
  // config declares each display's placement so the pointer moves between them
  // as laid out. Not a match field -- excluded from empty().
  std::optional<int32_t> layout_x;
  std::optional<int32_t> layout_y;

  // Touch device bonded to this view's display, matched by libinput
  // device-name substring. A touchscreen reports absolute coordinates in its
  // own space with no cursor, so unlike a mouse it can't be routed by pointer
  // position -- this names which panel feeds this view. Unset routes touch to
  // the primary view (preserving single-display behavior). Not a match field --
  // excluded from empty().
  std::optional<std::string> touch_device;

  // True when no match field is set: the bundle binds to the primary output
  // immediately (preserves single-view behavior). Layout position is not a
  // match constraint, so it does not affect this.
  [[nodiscard]] bool empty() const {
    return !output_id && !wl_name && !drm_connector && !edid_serial && !index;
  }
};

// The outcome of resolving a view's [view.output] against the live set.
//
// "no constraint" and "constrained but not satisfied" are different situations
// that a bare optional cannot tell apart, and they call for opposite handling:
// the first means the backend's own default is correct, the second means the
// display the config named is not there. Callers that cannot honor the second
// at least must not mistake it for the first.
struct OutputResolution {
  enum class Status {
    kUnconstrained,  // no [view.output]; the backend's default stands
    kBound,          // a constraint matched a live output
    kUnresolved,     // a constraint was set and nothing satisfied it
  };
  Status status = Status::kUnconstrained;
  std::string name;  // set only when kBound

  [[nodiscard]] bool bound() const { return status == Status::kBound; }
};

// What the output layer reported.
enum class OutputEvent {
  kAdded,
  kRemoved,
  kChanged,  // same output, new mode / EDID
};

// What an output event means for one view.
enum class OutputTransition {
  kNone,          // still where it belongs; nothing to do
  kMoved,         // the constraint now resolves to a different output
  kAppeared,      // the constraint resolves and the view is not on it yet
  kLost,          // the view's output no longer satisfies the constraint
  kReconfigured,  // same output, but its mode or EDID changed
};

// One change between two enumerations of a card's outputs.
struct OutputChange {
  OutputEvent event;
  // For kAdded and kChanged this is the output as it is now. For kRemoved
  // there is no "now" -- the port is disconnected or the connector is gone --
  // so it is the last state the output was seen in. Read it for the name and
  // nothing else on a removal.
  OutputInfo output;
};

/**
 * @brief Diff two enumerations of the same card's outputs.
 *
 * A DRM connector is not created and destroyed as displays come and go: it
 * stays enumerated and its `connected` flag moves, so an unplug is a
 * connected->disconnected transition rather than a disappearance. A connector
 * object vanishing entirely is a different thing (MST teardown, or the card
 * going away) and is reported as a removal too, since the output is equally
 * gone from the view's point of view.
 *
 * Only connected outputs are reported: a port that was disconnected and still
 * is has not changed, and nothing can bind to it either way.
 *
 * @param[in] before the previous enumeration
 * @param[in] after  the current one
 * @return the changes, in a stable order: removals first, then additions, then
 *         modifications, so a caller applying them in order never sees two
 *         outputs claiming the same name.
 */
[[nodiscard]] std::vector<OutputChange> DiffOutputs(
    const std::vector<OutputInfo>& before,
    const std::vector<OutputInfo>& after);

/**
 * @brief Decide what an output event means for a single view.
 *
 * Split out of the listener because this is the part that can be wrong: the
 * caller supplies where the view is and what its config now resolves to, and
 * the decision is a pure function of those. A view is only "lost" when a
 * constraint was set and went unsatisfied -- an unconstrained view keeps
 * whatever the backend chose, so its output vanishing is the backend's problem
 * to report, not a resolution failure.
 *
 * @param[in] current the view's actual output, from FlutterView::BoundOutput()
 * @param[in] wanted  the freshly re-resolved constraint
 * @param[in] event   what happened
 * @param[in] changed the output the event names; only meaningful for kChanged
 */
[[nodiscard]] OutputTransition ClassifyOutputTransition(
    const std::optional<std::string>& current,
    const OutputResolution& wanted,
    OutputEvent event,
    std::string_view changed);

// As ResolveOutput, but distinguishing kUnconstrained from kUnresolved.
[[nodiscard]] OutputResolution ResolveOutputDetailed(
    const OutputMatch& match,
    const std::vector<OutputInfo>& outputs,
    BackendFamily family);

// Resolve the OutputInfo::name a bundle should bind to, given the current live
// set, or nullopt if none matches (the bundle is then parked). `family` selects
// which OutputMatch name field is compared. Match order, most specific first:
//   0. output_id   — the connector's IHS_OUTPUT_NAME udev property, when an
//      integrator's rule sets one. Above the serial because it is assigned
//      deliberately, works on panels with no EDID, and is the only key that
//      holds across boards. An unmatched output_id parks rather than falling
//      through: naming a role and silently binding to some other display is
//      worse than not binding.
//   1. edid_serial — DRM only; only if present AND it uniquely identifies one
//      connected output. A duplicate/ambiguous serial warns and falls through.
//   2. name        — drm_connector (DRM) or wl_name (Wayland); the primary
//      path, and what makes identical monitors follow their port.
//   3. index       — deprecated and unstable; warns. Indexes the connected
//      outputs in enumeration order.
//   4. none        — nullopt (parked).
// Only `connected` outputs are eligible (you cannot present to an absent port).
[[nodiscard]] std::optional<std::string> ResolveOutput(
    const OutputMatch& match,
    const std::vector<OutputInfo>& outputs,
    BackendFamily family);

}  // namespace homescreen
