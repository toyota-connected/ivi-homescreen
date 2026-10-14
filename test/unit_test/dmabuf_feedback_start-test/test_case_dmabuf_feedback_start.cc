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

// Where the wayland_vulkan zero-copy path gets the compositor's dma-buf
// modifiers, against a mock compositor: per-surface feedback at v4+, the
// bind's format/modifier events at v3, nothing below.
//
// A hand-rolled libwayland server advertises zwp_linux_dmabuf_v1 at the
// version under test (and wl_compositor, for a surface to ask feedback for).
// At v3 it streams format/modifier events on bind; at v4+ it answers
// get_surface_feedback with a one-tranche scanout advertisement.

// clang-format off
#include "linux_dmabuf_client.hpp"  // generated (linux_dmabuf_unstable_v1::client)
#include <wl/linux_dmabuf.hpp>      // interface tables + wl_iface()
// clang-format on

#include "backend/wayland_vulkan/dmabuf_feedback_start.h"

extern "C" {
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
}

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace ldb = linux_dmabuf_unstable_v1::client;
using wl_vulkan::DmabufFeedbackSource;
using wl_vulkan::StartDmabufFeedback;

constexpr uint32_t kXRGB8888 = 0x34325258u;  // 'XR24'
constexpr uint64_t kLinear = 0;
constexpr uint64_t kTiled = 0x0300000000000001ull;  // a vendor modifier
constexpr uint64_t kMainDev = 0xE200ull;

const wl_interface* DmabufIface() {
  return &ldb::zwp_linux_dmabuf_v1_traits::wl_iface();
}
const wl_interface* FeedbackIface() {
  return &ldb::zwp_linux_dmabuf_feedback_v1_traits::wl_iface();
}

// ---- Server: the compositor --------------------------------------------

struct Server {
  std::vector<int> tables;  // closed after the client is gone
};

void SrvDestroy(wl_client* /*c*/, wl_resource* r) {
  wl_resource_destroy(r);
}

struct SurfaceImpl {
  void (*destroy)(wl_client*, wl_resource*);
};
constexpr SurfaceImpl kSurfaceImpl{&SrvDestroy};

struct CompositorImpl {
  void (*create_surface)(wl_client*, wl_resource*, uint32_t);
  void (*create_region)(wl_client*, wl_resource*, uint32_t);
};

void SrvCreateSurface(wl_client* c, wl_resource* comp, uint32_t id) {
  wl_resource* s =
      wl_resource_create(c, &wl_surface_interface,
                         wl_resource_get_version(comp), static_cast<int>(id));
  wl_resource_set_implementation(s, &kSurfaceImpl, nullptr, nullptr);
}
constexpr CompositorImpl kCompositorImpl{&SrvCreateSurface, nullptr};

void CompositorBind(wl_client* c,
                    void* /*data*/,
                    uint32_t version,
                    uint32_t id) {
  wl_resource* r =
      wl_resource_create(c, &wl_compositor_interface, static_cast<int>(version),
                         static_cast<int>(id));
  wl_resource_set_implementation(r, &kCompositorImpl, nullptr, nullptr);
}

struct FeedbackImpl {
  void (*destroy)(wl_client*, wl_resource*);
};
constexpr FeedbackImpl kFeedbackImpl{&SrvDestroy};

void PostArray(wl_resource* r, uint32_t op, const void* data, size_t size) {
  wl_array a{};
  wl_array_init(&a);
  std::memcpy(wl_array_add(&a, size), data, size);
  wl_resource_post_event(r, op, &a);
  wl_array_release(&a);
}

// One scanout tranche: XRGB8888 tiled.
void SrvGetSurfaceFeedback(wl_client* c,
                           wl_resource* dmabuf,
                           uint32_t id,
                           wl_resource* /*surface*/) {
  auto* srv = static_cast<Server*>(wl_resource_get_user_data(dmabuf));
  wl_resource* fb =
      wl_resource_create(c, FeedbackIface(), wl_resource_get_version(dmabuf),
                         static_cast<int>(id));
  wl_resource_set_implementation(fb, &kFeedbackImpl, nullptr, nullptr);

  struct Entry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
  };
  const Entry entry{kXRGB8888, 0, kTiled};
  const int fd = memfd_create("ftable", MFD_CLOEXEC);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(ftruncate(fd, sizeof(entry)), 0);
  ASSERT_EQ(pwrite(fd, &entry, sizeof(entry), 0),
            static_cast<ssize_t>(sizeof(entry)));
  srv->tables.push_back(fd);

  using Evt = ldb::zwp_linux_dmabuf_feedback_v1_traits::Evt;
  wl_resource_post_event(fb, Evt::FormatTable, fd,
                         static_cast<uint32_t>(sizeof(entry)));
  PostArray(fb, Evt::MainDevice, &kMainDev, sizeof(kMainDev));
  PostArray(fb, Evt::TrancheTargetDevice, &kMainDev, sizeof(kMainDev));
  wl_resource_post_event(fb, Evt::TrancheFlags, 1u);  // scanout
  const uint16_t index = 0;
  PostArray(fb, Evt::TrancheFormats, &index, sizeof(index));
  wl_resource_post_event(fb, Evt::TrancheDone);
  wl_resource_post_event(fb, Evt::Done);
}

struct DmabufImpl {
  void (*destroy)(wl_client*, wl_resource*);
  void (*create_params)(wl_client*, wl_resource*, uint32_t);
  void (*get_default_feedback)(wl_client*, wl_resource*, uint32_t);
  void (*get_surface_feedback)(wl_client*,
                               wl_resource*,
                               uint32_t,
                               wl_resource*);
};
constexpr DmabufImpl kDmabufImpl{&SrvDestroy, nullptr, nullptr,
                                 &SrvGetSurfaceFeedback};

void DmabufBind(wl_client* c, void* data, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(
      c, DmabufIface(), static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_implementation(r, &kDmabufImpl, data, nullptr);
  // Before v4 the formats come on bind, and from v3 their modifiers too;
  // v4+ leaves both to the feedback objects.
  using Evt = ldb::zwp_linux_dmabuf_v1_traits::Evt;
  if (version <= 3) {
    wl_resource_post_event(r, Evt::Format, kXRGB8888);
  }
  if (version == 3) {
    for (const uint64_t m : {kTiled, kLinear}) {
      wl_resource_post_event(r, Evt::Modifier, kXRGB8888,
                             static_cast<uint32_t>(m >> 32),
                             static_cast<uint32_t>(m & 0xffffffffu));
    }
  }
}

// ---- Client: what the backend does ---------------------------------------

struct App {
  wl::DmabufFeedback<App> feedback;
  int snapshots = 0;
  void OnDmabufFeedback(const wl::FeedbackSnapshot& /*s*/) { ++snapshots; }
  void OnDmabufLegacyFormat(uint32_t /*fourcc*/, uint64_t /*mod*/) {}
};

struct Result {
  DmabufFeedbackSource source = DmabufFeedbackSource::kNone;
  int snapshots = 0;
  std::vector<uint64_t> modifiers;
  std::vector<uint64_t> scanout;
  std::vector<uint64_t> all;  // INVALID included
};

// Runs StartDmabufFeedback against a compositor advertising the dma-buf
// global at @p version, for a real surface or, with @p with_surface false,
// none.
Result StartAgainst(uint32_t version, bool with_surface = true) {
  Result result;
  std::array<int, 2> sv{-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  Server srv;
  wl_display* server = wl_display_create();
  EXPECT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, &wl_compositor_interface, 4, nullptr,
                   &CompositorBind);
  wl_global_create(server, DmabufIface(), static_cast<int>(version), &srv,
                   &DmabufBind);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  EXPECT_NE(client, nullptr);
  if (client != nullptr) {
    {
      App app;
      wl::CRegistry registry;
      EXPECT_TRUE(registry.Create(client));
      uint32_t compositor_name = 0;
      registry.OnGlobal(
          [&](wl::CRegistry&, uint32_t n, std::string_view iface, uint32_t v) {
            if (iface == ldb::zwp_linux_dmabuf_v1_traits::interface_name) {
              app.feedback.Record(n, v);
            } else if (iface == "wl_compositor") {
              compositor_name = n;
            }
          });
      EXPECT_NE(wl_display_roundtrip(client), -1);

      wl_registry* raw = wl_display_get_registry(client);
      auto* compositor = static_cast<wl_compositor*>(
          wl_registry_bind(raw, compositor_name, &wl_compositor_interface, 4));
      wl_surface* surface =
          with_surface ? wl_compositor_create_surface(compositor) : nullptr;

      result.source = StartDmabufFeedback(app.feedback, registry, &app, client,
                                          reinterpret_cast<wl_proxy*>(surface));
      result.snapshots = app.snapshots;
      // A format event alone stands for DRM_FORMAT_MOD_INVALID, which no
      // device exports; leave it out.
      for (const uint64_t m : app.feedback.Current().ModifiersFor(kXRGB8888)) {
        if (m != wl::DmabufFeedback<App>::kModifierInvalid) {
          result.modifiers.push_back(m);
        }
      }
      result.scanout = app.feedback.Current().ScanoutModifiersFor(kXRGB8888);
      result.all = app.feedback.Current().ModifiersFor(kXRGB8888);

      app.feedback.Release();
      if (surface != nullptr) {
        wl_surface_destroy(surface);
      }
      wl_compositor_destroy(compositor);
      wl_registry_destroy(raw);
      wl_display_roundtrip(client);
    }
    wl_display_disconnect(client);
  }
  wl_display_terminate(server);
  server_thread.join();
  wl_display_destroy_clients(server);
  wl_display_destroy(server);
  for (const int fd : srv.tables) {
    close(fd);
  }
  return result;
}

}  // namespace

TEST(DmabufFeedbackStart, V4UsesPerSurfaceFeedback) {
  const Result r = StartAgainst(4);
  EXPECT_EQ(r.source, DmabufFeedbackSource::kSurface);
  EXPECT_EQ(r.snapshots, 1);
  EXPECT_EQ(r.modifiers, std::vector<uint64_t>{kTiled});
  EXPECT_EQ(r.scanout, std::vector<uint64_t>{kTiled});
}

TEST(DmabufFeedbackStart, V3UsesTheFormatEvents) {
  const Result r = StartAgainst(3);
  EXPECT_EQ(r.source, DmabufFeedbackSource::kFormatEvents);
  EXPECT_EQ(r.snapshots, 1);
  EXPECT_EQ(r.modifiers, (std::vector<uint64_t>{kTiled, kLinear}));
  EXPECT_TRUE(r.scanout.empty()) << "v3 has no scanout flag";
  // The format event's own entry is DRM_FORMAT_MOD_INVALID; no device
  // exports it, so modifier negotiation drops it.
  EXPECT_EQ(r.all,
            (std::vector<uint64_t>{wl::DmabufFeedback<App>::kModifierInvalid,
                                   kTiled, kLinear}));
}

TEST(DmabufFeedbackStart, BelowV3NoModifiersAreKnown) {
  const Result r = StartAgainst(2);
  EXPECT_EQ(r.source, DmabufFeedbackSource::kNone);
  EXPECT_EQ(r.snapshots, 0);
  EXPECT_TRUE(r.modifiers.empty());
}

TEST(DmabufFeedbackStart, V4WithoutSurfaceFeedbackIsNotTakenForV3) {
  // A v4 compositor sends no format events: when per-surface feedback can't
  // start, there is nothing to build modifiers from.
  const Result r = StartAgainst(4, /*with_surface=*/false);
  EXPECT_EQ(r.source, DmabufFeedbackSource::kNone);
  EXPECT_EQ(r.snapshots, 0);
}
