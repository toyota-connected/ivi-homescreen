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

#include "backend/software/sink_factory.h"
#include "logging/logging.h"

#include <cstdlib>
#include <string>

#include "backend/software/file_sink.h"
#include "backend/software/memory_sink.h"
#include "backend/software/none_sink.h"
#include "logging.h"

#if BUILD_SOFTWARE_SINK_DRM
#include "backend/software/drm_dumb_sink.h"
#endif

#if BUILD_SOFTWARE_SINK_FBDEV
#include "backend/software/fbdev_sink.h"
#endif

std::unique_ptr<ISurfaceSink> MakeSinkFromSpec(const std::string_view spec) {
  if (spec.empty() || spec == "none") {
    spdlog::info("[SoftwareBackend] sink: none (frames discarded)");
    return std::make_unique<NoneSink>();
  }
  if (spec == "memory") {
    spdlog::info("[SoftwareBackend] sink: memory (in-process snapshot)");
    return std::make_unique<MemorySink>();
  }
  // file:<pattern>
  constexpr std::string_view kFilePrefix = "file:";
  if (spec.rfind(kFilePrefix, 0) == 0) {
    std::string pattern(spec.substr(kFilePrefix.size()));
    if (pattern.empty()) {
      spdlog::warn(
          "[SoftwareBackend] sink spec 'file:' has empty pattern; "
          "falling back to NoneSink");
      return std::make_unique<NoneSink>();
    }
    spdlog::info("[SoftwareBackend] sink: file (pattern='{}')", pattern);
    return std::make_unique<FileSink>(std::move(pattern));
  }

  // drm-dumb:<device>  (device is optional; defaults to /dev/dri/card0)
  constexpr std::string_view kDrmPrefix = "drm-dumb:";
  if (spec == "drm-dumb" || spec.rfind(kDrmPrefix, 0) == 0) {
#if BUILD_SOFTWARE_SINK_DRM
    const std::string device(spec == "drm-dumb"
                                 ? std::string_view{}
                                 : spec.substr(kDrmPrefix.size()));
    auto drm_sink = DrmDumbSink::Create(device);
    if (drm_sink) {
      spdlog::info(
          "[SoftwareBackend] sink: drm-dumb (device='{}', {}x{}@{:.2f}Hz)",
          device.empty() ? std::string("/dev/dri/card0") : device,
          drm_sink->mode_width(), drm_sink->mode_height(),
          drm_sink->refresh_rate_hz());
      return drm_sink;
    }
    spdlog::warn(
        "[SoftwareBackend] drm-dumb sink failed to initialize; "
        "falling back to NoneSink");
    return std::make_unique<NoneSink>();
#else
    spdlog::warn(
        "[SoftwareBackend] drm-dumb sink requested but compiled without "
        "BUILD_SOFTWARE_SINK_DRM; falling back to NoneSink");
    return std::make_unique<NoneSink>();
#endif
  }

  // fbdev:<device>  (device is optional; defaults to /dev/fb0)
  constexpr std::string_view kFbDevPrefix = "fbdev:";
  if (spec == "fbdev" || spec.rfind(kFbDevPrefix, 0) == 0) {
#if BUILD_SOFTWARE_SINK_FBDEV
    const std::string device(spec == "fbdev"
                                 ? std::string_view{}
                                 : spec.substr(kFbDevPrefix.size()));
    auto fb_sink = FbDevSink::Create(device);
    if (fb_sink) {
      spdlog::info("[SoftwareBackend] sink: fbdev (device='{}', {}x{})",
                   device.empty() ? std::string("/dev/fb0") : device,
                   fb_sink->fb_width(), fb_sink->fb_height());
      return fb_sink;
    }
    spdlog::warn(
        "[SoftwareBackend] fbdev sink failed to initialize; "
        "falling back to NoneSink");
    return std::make_unique<NoneSink>();
#else
    spdlog::warn(
        "[SoftwareBackend] fbdev sink requested but compiled without "
        "BUILD_SOFTWARE_SINK_FBDEV; falling back to NoneSink");
    return std::make_unique<NoneSink>();
#endif
  }

  spdlog::warn(
      "[SoftwareBackend] unrecognized sink spec '{}' (valid: none | memory | "
      "file:<pattern> | fbdev[:<device>] | drm-dumb[:<device>]); falling "
      "back to NoneSink",
      std::string(spec));
  return std::make_unique<NoneSink>();
}

std::unique_ptr<ISurfaceSink> MakeSinkFromEnv() {
  const char* env = std::getenv("IVI_SW_SINK");
  return MakeSinkFromSpec(env != nullptr ? std::string_view(env)
                                         : std::string_view{});
}
