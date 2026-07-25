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
#include "config/common.h"  // BUILD_SOFTWARE_SINK_DRM / _FBDEV gates below
#include "logging/logging.h"

// Defines BUILD_SOFTWARE_SINK_DRM / BUILD_SOFTWARE_SINK_FBDEV, which gate both
// the sink includes and the factory branches below. Nothing else this file
// includes pulls it in, and an undefined identifier in #if is 0, so without it
// every drm-dumb / fbdev spec silently falls back to NoneSink.
#include "config/common.h"

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

#if BUILD_SOFTWARE_SINK_ENCODER
#include "backend/software/encoder_sink.h"
#endif

std::unique_ptr<ISurfaceSink> MakeSinkFromSpec(
    const std::string_view spec,
    const std::string_view drm_device_hint) {
  if (spec.empty() || spec == "none") {
    ihs::log::info("[SoftwareBackend] sink: none (frames discarded)");
    return std::make_unique<NoneSink>();
  }
  if (spec == "memory") {
    ihs::log::info("[SoftwareBackend] sink: memory (in-process snapshot)");
    return std::make_unique<MemorySink>();
  }
  // file:<pattern>
  constexpr std::string_view kFilePrefix = "file:";
  if (spec.rfind(kFilePrefix, 0) == 0) {
    std::string pattern(spec.substr(kFilePrefix.size()));
    if (pattern.empty()) {
      ihs::log::warn(
          "[SoftwareBackend] sink spec 'file:' has empty pattern; "
          "falling back to NoneSink");
      return std::make_unique<NoneSink>();
    }
    ihs::log::info("[SoftwareBackend] sink: file (pattern='{}')", pattern);
    return std::make_unique<FileSink>(std::move(pattern));
  }

  // drm-dumb[:<device>]  — explicit device wins; else the --drm-device hint;
  // else empty, which makes DrmDumbSink probe the first usable card.
  constexpr std::string_view kDrmPrefix = "drm-dumb:";
  if (spec == "drm-dumb" || spec.rfind(kDrmPrefix, 0) == 0) {
#if BUILD_SOFTWARE_SINK_DRM
    const std::string device(
        spec == "drm-dumb" ? drm_device_hint : spec.substr(kDrmPrefix.size()));
    auto drm_sink = DrmDumbSink::Create(device);
    if (drm_sink) {
      ihs::log::info(
          "[SoftwareBackend] sink: drm-dumb (device='{}', {}x{}@{:.2f}Hz)",
          device.empty() ? std::string("(probed)") : device,
          drm_sink->mode_width(), drm_sink->mode_height(),
          drm_sink->refresh_rate_hz());
      return drm_sink;
    }
    ihs::log::warn(
        "[SoftwareBackend] drm-dumb sink failed to initialize; "
        "falling back to NoneSink");
    return std::make_unique<NoneSink>();
#else
    ihs::log::warn(
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
      ihs::log::info("[SoftwareBackend] sink: fbdev (device='{}', {}x{})",
                     device.empty() ? std::string("/dev/fb0") : device,
                     fb_sink->fb_width(), fb_sink->fb_height());
      return fb_sink;
    }
    ihs::log::warn(
        "[SoftwareBackend] fbdev sink failed to initialize; "
        "falling back to NoneSink");
    return std::make_unique<NoneSink>();
#else
    ihs::log::warn(
        "[SoftwareBackend] fbdev sink requested but compiled without "
        "BUILD_SOFTWARE_SINK_FBDEV; falling back to NoneSink");
    return std::make_unique<NoneSink>();
#endif
  }

  // encoder[:<consumer-spec>]  — pack each present to NV12 and hand it to a
  // consumer (today: file:<path> to the V4L2 hardware encoder).
  constexpr std::string_view kEncoderPrefix = "encoder:";
  if (spec == "encoder" || spec.rfind(kEncoderPrefix, 0) == 0) {
#if BUILD_SOFTWARE_SINK_ENCODER
    const std::string_view consumer_spec =
        spec == "encoder" ? std::string_view{}
                          : spec.substr(kEncoderPrefix.size());
    auto consumer = MakeNv12Consumer(consumer_spec);
    if (consumer) {
      ihs::log::info("[SoftwareBackend] sink: encoder (consumer='{}')",
                     std::string(consumer_spec));
      return std::make_unique<EncoderSink>(std::move(consumer));
    }
    ihs::log::warn(
        "[SoftwareBackend] encoder sink consumer '{}' failed; falling back to "
        "NoneSink",
        std::string(consumer_spec));
    return std::make_unique<NoneSink>();
#else
    ihs::log::warn(
        "[SoftwareBackend] encoder sink requested but compiled without "
        "BUILD_SOFTWARE_SINK_ENCODER; falling back to NoneSink");
    return std::make_unique<NoneSink>();
#endif
  }

  ihs::log::warn(
      "[SoftwareBackend] unrecognized sink spec '{}' (valid: none | memory | "
      "file:<pattern> | fbdev[:<device>] | drm-dumb[:<device>] | "
      "encoder[:file:<path>]); falling back to NoneSink",
      std::string(spec));
  return std::make_unique<NoneSink>();
}

std::unique_ptr<ISurfaceSink> MakeSinkFromEnv(
    const std::string_view drm_device_hint) {
  const char* env = std::getenv("IVI_SW_SINK");
  return MakeSinkFromSpec(
      env != nullptr ? std::string_view(env) : std::string_view{},
      drm_device_hint);
}
