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

#include "backend/drm_kms_egl/drm_capture.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <drm-cxx/capture/png.hpp>
#include <drm-cxx/capture/snapshot.hpp>

#include "config/common.h"
#include "logging.h"

namespace homescreen {
namespace {

// Process-global state — SIGUSR1 is process-wide and any of our
// DrmCapture instances would observe it. Only one DrmBackend (and
// therefore one DrmCapture) exists at a time in practice, so the
// singleton-flag pattern is sufficient.
std::atomic<bool> g_pending{false};
std::once_flag g_installed;

extern "C" void HandleSigusr1(int /*sig*/) {
  // Async-signal-safe: only the atomic store. No malloc, no logging.
  g_pending.store(true, std::memory_order_release);
}

void InstallHandler() {
  struct sigaction sa{};
  sa.sa_handler = &HandleSigusr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  ::sigaction(SIGUSR1, &sa, nullptr);
}

// Resolve the output directory. Prefers $XDG_RUNTIME_DIR (systemd-logind
// guarantees mode 0700 owned by the running user — closes both the
// symlink-trap and the world-readable-disclosure surfaces of /tmp). On
// systems without XDG_RUNTIME_DIR (bare-cron jobs, some embedded inits)
// falls back to /tmp; callers must lstat-refuse-existing in that mode.
std::string ResolveOutputDir() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg == nullptr || *xdg == '\0') {
    return "/tmp";
  }
  std::string dir = std::string(xdg) + "/" + kApplicationName;
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    spdlog::warn("[DrmCapture] mkdir({}): {}; falling back to /tmp (less safe)",
                 dir, std::strerror(errno));
    return "/tmp";
  }
  return dir;
}

}  // namespace

std::unique_ptr<DrmCapture> DrmCapture::Create() {
  const char* gate = std::getenv("IVI_DRM_CAPTURE");
  if (gate == nullptr || std::string_view(gate) != "1") {
    return nullptr;
  }
  std::call_once(g_installed, &InstallHandler);
  spdlog::info(
      "[DrmCapture] SIGUSR1 capture armed; "
      "kill -USR1 {} drops <dir>/{}-snapshot-<ms>.png",
      ::getpid(), kApplicationName);
  return std::unique_ptr<DrmCapture>(new DrmCapture());
}

DrmCapture::DrmCapture() = default;

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void DrmCapture::MaybeCapture(const drm::Device& dev,
                              const std::uint32_t crtc_id) {
  if (!g_pending.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  const std::string dir = ResolveOutputDir();
  const std::string path = dir + "/" + kApplicationName + "-snapshot-" +
                           std::to_string(epoch_ms) + ".png";

  // Symlink-trap protection. Blend2D's write_to_file uses fopen("wb")
  // which would follow a same-UID-planted symlink at our path. Refuse
  // if anything already exists at the path. In XDG_RUNTIME_DIR this is
  // belt-and-suspenders (the dir is 0700); in /tmp it's the only
  // defense.
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0) {
    spdlog::warn(
        "[DrmCapture] refusing to write {}: path already exists "
        "(symlink-trap protection)",
        path);
    return;
  }

  auto img = drm::capture::snapshot(dev, crtc_id);
  if (!img) {
    spdlog::warn("[DrmCapture] snapshot(crtc={}): {}", crtc_id,
                 img.error().message());
    return;
  }
  if (auto r = drm::capture::write_png(*img, path); !r) {
    spdlog::warn("[DrmCapture] write_png({}): {}", path, r.error().message());
    return;
  }
  spdlog::info("[DrmCapture] wrote {} ({}x{})", path, img->width(),
               img->height());
}

}  // namespace homescreen
