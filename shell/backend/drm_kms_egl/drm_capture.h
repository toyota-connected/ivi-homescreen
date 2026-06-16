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
#include <memory>

namespace drm {
class Device;
}

namespace homescreen {

// SIGUSR1-driven CRTC snapshot debug helper.
//
// When IVI_DRM_CAPTURE=1 at startup, Create() installs an async-signal-
// safe SIGUSR1 handler that sets an atomic "capture pending" flag. On
// the next Present (or PresentLayers) call, MaybeCapture() observes the
// flag and runs drm::capture::snapshot + drm::capture::write_png to
// <$XDG_RUNTIME_DIR/$APP/>$APP-snapshot-<unix-ms>.png (fallback /tmp
// with lstat-refusal), where $APP is the executable name from CMake's
// EXE_OUTPUT_NAME. That frame stutters by tens of milliseconds — fine
// for an opt-in field-debug tool, never enabled in production.
//
// When the env var is unset (the default), Create() returns nullptr and
// every consumer falls through with a single nullptr check per frame.
//
// Requires drm-cxx's drm::capture module (depends on Blend2D). When the
// homescreen build was configured without HAVE_DRM_CAPTURE, this file
// is not compiled and DrmBackend skips the wire-up entirely.
class DrmCapture {
 public:
  // Returns nullptr when IVI_DRM_CAPTURE != "1". Installs the SIGUSR1
  // handler on first non-null return; the handler stays installed for
  // the lifetime of the process (SA_RESTART) so callers don't need to
  // worry about uninstall ordering.
  static std::unique_ptr<DrmCapture> Create();

  DrmCapture(const DrmCapture&) = delete;
  DrmCapture& operator=(const DrmCapture&) = delete;
  ~DrmCapture() = default;

  // Called once per frame from each Present path. Cheap when no
  // capture is pending (one atomic load). When pending, drains the
  // flag and runs snapshot + write_png against the given device/CRTC,
  // logging the result. Failures are non-fatal — the frame still
  // presents normally.
  //
  // Kept non-static even though no instance state is touched: the
  // presence of a DrmCapture object (created only when armed) IS the
  // gate — DrmBackend::MaybeCaptureSnapshot's nullptr check enforces
  // "Create() was called" before MaybeCapture is invoked.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void MaybeCapture(const drm::Device& dev, std::uint32_t crtc_id);

 private:
  DrmCapture();
};

}  // namespace homescreen
