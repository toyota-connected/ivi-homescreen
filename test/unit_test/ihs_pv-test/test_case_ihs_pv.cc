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

/*
 * Unit tests for the ihs_shared platform-view surface (ihs/platform_view.h +
 * ihs/platform_view_host.h). libihs_shared is a thin forwarder over a host, so
 * these link it directly and assert that each ihs_pv_* entry point forwards to
 * the installed mock host, that the pure best-to-floor negotiate scoring picks
 * the right kind, and that the surface degrades cleanly with no host — no
 * shell, no backend, no compositor.
 */

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs.h"
#include "ihs/platform_view.h"
#include "ihs/platform_view_host.h"

namespace {

// ---- mock host (stands in for the shell) -----------------------------------

struct MockHost {
  uint32_t caps_kinds =
      IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM;
  uint8_t explicit_sync = 0;

  int register_calls = 0;
  int unregister_calls = 0;
  int query_calls = 0;
  int grant_calls = 0;
  int submit_calls = 0;
  uint32_t last_grant_kind = IHS_PV_KIND_NONE;
  // Stands in for a host whose planes cannot scan out what was negotiated:
  // when set, grant rewrites the modifier and negotiate must report it back.
  bool substitute_modifier = false;
  uint64_t substitute_with = 0;
  uint64_t last_grant_modifier_in = 0;
  IhsPvFactory last_factory = nullptr;
  IhsPlatformView* last_submit_view = nullptr;
};

int mock_register(void* u,
                  const char* /*view_type*/,
                  IhsPvFactory factory,
                  void* /*factory_user_data*/) {
  auto* m = static_cast<MockHost*>(u);
  ++m->register_calls;
  m->last_factory = factory;
  return IHS_PV_OK;
}

void mock_unregister(void* u, const char* /*view_type*/) {
  ++static_cast<MockHost*>(u)->unregister_calls;
}

int mock_query(void* u, IhsPvCapabilities* out) {
  auto* m = static_cast<MockHost*>(u);
  ++m->query_calls;
  out->backend_key = "mock";
  out->kinds = m->caps_kinds;
  out->explicit_sync = m->explicit_sync;
  return IHS_PV_OK;
}

int mock_vulkan(void* /*u*/, IhsVulkanContext* out) {
  out->device = reinterpret_cast<void*>(0x1234);
  return IHS_PV_OK;
}

int mock_grant(void* u,
               IhsPlatformView* /*view*/,
               uint32_t kind,
               IhsFormatModifier* fmt,
               uint32_t* out_plane,
               int* out_shm_fd,
               size_t* out_shm_stride) {
  auto* m = static_cast<MockHost*>(u);
  ++m->grant_calls;
  m->last_grant_kind = kind;
  if (fmt != nullptr) {
    m->last_grant_modifier_in = fmt->modifier;
    if (m->substitute_modifier) {
      fmt->modifier = m->substitute_with;
    }
  }
  if (kind == IHS_PV_KIND_DRM_PLANE) {
    *out_plane = 42;
  } else if (kind == IHS_PV_KIND_SOFTWARE_SHM) {
    *out_shm_fd = 7;
    *out_shm_stride = 256;
  }
  return IHS_PV_OK;
}

void mock_revoke(void* /*u*/, IhsPlatformView* /*view*/) {}

uint32_t mock_grant_plane(void* /*u*/, IhsPlatformView* /*view*/) {
  return 42;
}

int mock_grant_shm(void* /*u*/, IhsPlatformView* /*view*/, size_t* out_stride) {
  if (out_stride != nullptr) {
    *out_stride = 256;
  }
  return 7;
}

int mock_submit(void* u,
                IhsPlatformView* view,
                const IhsFrame* /*frame*/,
                int /*acquire_fence_fd*/,
                int* out_release_fence_fd) {
  auto* m = static_cast<MockHost*>(u);
  ++m->submit_calls;
  m->last_submit_view = view;
  if (out_release_fence_fd != nullptr) {
    *out_release_fence_fd = -1;
  }
  return IHS_PV_OK;
}

IhsPvHost make_host(MockHost* m) {
  IhsPvHost h{};
  h.struct_size = sizeof(h);
  h.user_data = m;
  h.register_factory = mock_register;
  h.unregister_factory = mock_unregister;
  h.query_capabilities = mock_query;
  h.vulkan_context = mock_vulkan;
  h.egl_context = nullptr;
  h.grant = mock_grant;
  h.revoke = mock_revoke;
  h.grant_drm_plane_id = mock_grant_plane;
  h.grant_shm_fd = mock_grant_shm;
  h.submit = mock_submit;
  return h;
}

// A stand-in for the shell-owned opaque view handle: libihs_shared only passes
// it through, so any non-null pointer serves.
int g_view_token = 0;
IhsPlatformView* fake_view() {
  return reinterpret_cast<IhsPlatformView*>(&g_view_token);
}

int dummy_factory(const IhsPvCreateInfo*,
                  void*,
                  IhsPlatformView*,
                  IhsPvCallbacks*,
                  void**) {
  return IHS_PV_OK;
}

void detach_host() {
  ihs_pv_set_host(nullptr);
}

IhsPvRequirements make_req(uint32_t kinds) {
  IhsPvRequirements req{};
  req.struct_size = sizeof(req);
  req.kinds = kinds;
  return req;
}

}  // namespace

// The sub-table is present and its pointers alias the flat entry points.
TEST(IhsPvSurface, SubTableAliasesFlatEntryPoints) {
  const IhsApi* api = ihs_get_api(IHS_SHARED_ABI_VERSION);
  ASSERT_NE(api, nullptr);
  ASSERT_NE(api->platform_view, nullptr);
  EXPECT_EQ(api->platform_view->register_factory, &ihs_pv_register_factory);
  EXPECT_EQ(api->platform_view->query_capabilities, &ihs_pv_query_capabilities);
  EXPECT_EQ(api->platform_view->negotiate, &ihs_pv_negotiate);
  EXPECT_EQ(api->platform_view->submit, &ihs_pv_submit);
}

// With no host installed, every stateful call reports cleanly.
TEST(IhsPvSurface, HeadlessDegradesCleanly) {
  detach_host();

  EXPECT_EQ(ihs_pv_register_factory("views/x", dummy_factory, nullptr),
            IHS_PV_ERR_NO_REGISTRY);

  IhsPvCapabilities caps{};
  caps.struct_size = sizeof(caps);
  EXPECT_EQ(ihs_pv_query_capabilities(&caps), IHS_PV_ERR_NO_REGISTRY);

  IhsVulkanContext vk{};
  vk.struct_size = sizeof(vk);
  EXPECT_EQ(ihs_pv_vulkan_context(&vk), IHS_PV_ERR_UNSUPPORTED);

  IhsPvRequirements req = make_req(IHS_PV_KIND_SOFTWARE_SHM);
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  EXPECT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant),
            IHS_PV_ERR_NO_REGISTRY);
}

TEST(IhsPvSurface, NullArgumentsRejected) {
  EXPECT_EQ(ihs_pv_register_factory(nullptr, dummy_factory, nullptr),
            IHS_PV_ERR_INVALID);
  EXPECT_EQ(ihs_pv_register_factory("t", nullptr, nullptr), IHS_PV_ERR_INVALID);
  EXPECT_EQ(ihs_pv_query_capabilities(nullptr), IHS_PV_ERR_INVALID);
  EXPECT_EQ(ihs_pv_negotiate(nullptr, nullptr, nullptr), IHS_PV_ERR_INVALID);
  EXPECT_EQ(ihs_pv_submit(nullptr, nullptr, -1, nullptr), IHS_PV_ERR_INVALID);
}

// register/unregister forward straight to the host (the shell owns the table).
TEST(IhsPvSurface, RegisterForwardsToHost) {
  MockHost host_state;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  EXPECT_EQ(ihs_pv_register_factory("views/mock", dummy_factory, nullptr),
            IHS_PV_OK);
  EXPECT_EQ(host_state.register_calls, 1);
  EXPECT_EQ(host_state.last_factory, &dummy_factory);

  ihs_pv_unregister_factory("views/mock");
  EXPECT_EQ(host_state.unregister_calls, 1);

  detach_host();
}

// Explicit sync is granted only when the backend advertises the capability.
// The EGL backends advertise it when the display has
// EGL_ANDROID_native_fence_sync, so the GL-composite path can wait on the
// producer's sync_file before sampling (#513); a display without it still
// downgrades every producer here.
TEST(IhsPvSurface, ExplicitSyncGrantedWhenBackendAdvertisesIt) {
  MockHost host_state;
  host_state.explicit_sync = 1;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req = make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  req.sync = IHS_PV_SYNC_EXPLICIT_PREFERRED;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.sync, static_cast<uint32_t>(IHS_PV_SYNC_EXPLICIT_PREFERRED));

  detach_host();
}

// PREFERRED is documented as "explicit if available, silently implicit if
// not", so a backend without the capability downgrades rather than failing.
TEST(IhsPvSurface, ExplicitSyncPreferredDowngradesWhenUnavailable) {
  MockHost host_state;
  host_state.explicit_sync = 0;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req = make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  req.sync = IHS_PV_SYNC_EXPLICIT_PREFERRED;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.sync, static_cast<uint32_t>(IHS_PV_SYNC_IMPLICIT));

  detach_host();
}

// A grant honors at most what was asked for: a plugin that asked for implicit
// sync is never handed an explicit-sync grant it is not prepared to service.
TEST(IhsPvSurface, ImplicitRequestIsNeverUpgraded) {
  MockHost host_state;
  host_state.explicit_sync = 1;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req = make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  req.sync = IHS_PV_SYNC_IMPLICIT;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.sync, static_cast<uint32_t>(IHS_PV_SYNC_IMPLICIT));

  detach_host();
}

// REQUIRED is a demand: a backend with no explicit sync fails the negotiation
// rather than quietly handing back an implicit grant.
TEST(IhsPvSurface, ExplicitSyncRequiredFailsWhenUnavailable) {
  MockHost host_state;
  host_state.explicit_sync = 0;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req = make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  req.sync = IHS_PV_SYNC_EXPLICIT_REQUIRED;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  EXPECT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant),
            IHS_PV_ERR_UNSUPPORTED);
  // Refused before grant(), so the host allocated no plane id or shm fd for a
  // negotiation it could not honor.
  EXPECT_EQ(host_state.grant_calls, 0);

  detach_host();
}

TEST(IhsPvSurface, ExplicitSyncRequiredGrantedWhenAvailable) {
  MockHost host_state;
  host_state.explicit_sync = 1;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req = make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  req.sync = IHS_PV_SYNC_EXPLICIT_REQUIRED;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.sync, static_cast<uint32_t>(IHS_PV_SYNC_EXPLICIT_REQUIRED));
  EXPECT_EQ(host_state.grant_calls, 1);

  detach_host();
}

// query_capabilities/vulkan_context forward and surface the host's answers.
TEST(IhsPvSurface, ContextQueriesForwardToHost) {
  MockHost host_state;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvCapabilities caps{};
  caps.struct_size = sizeof(caps);
  ASSERT_EQ(ihs_pv_query_capabilities(&caps), IHS_PV_OK);
  EXPECT_EQ(host_state.query_calls, 1);
  EXPECT_EQ(caps.kinds,
            IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM);

  IhsVulkanContext vk{};
  vk.struct_size = sizeof(vk);
  ASSERT_EQ(ihs_pv_vulkan_context(&vk), IHS_PV_OK);
  EXPECT_EQ(vk.device, reinterpret_cast<void*>(0x1234));

  // egl_context is absent on this host -> unsupported.
  IhsEglContext egl{};
  egl.struct_size = sizeof(egl);
  EXPECT_EQ(ihs_pv_egl_context(&egl), IHS_PV_ERR_UNSUPPORTED);

  detach_host();
}

// Negotiate scores best-to-floor: TEXTURE beats SHM when both are on offer.
TEST(IhsPvSurface, NegotiatePicksBestKind) {
  MockHost host_state;  // caps = TEXTURE | SHM
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req =
      make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM);
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.granted_kind, IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  EXPECT_EQ(host_state.last_grant_kind, IHS_PV_KIND_TEXTURE_DMABUF_IMPORT);
  EXPECT_EQ(host_state.grant_calls, 1);

  detach_host();
}

// grant may replace the modifier (1.15) and negotiate reports what came back,
// so the grant describes the buffer the producer will actually allocate. The
// case this exists for: a DRM_PLANE grant whose negotiated modifier no plane
// scans out (ivi-homescreen#642).
TEST(IhsPvSurface, GrantMaySubstituteTheModifier) {
  MockHost host_state;
  host_state.caps_kinds = IHS_PV_KIND_DRM_PLANE;
  host_state.substitute_modifier = true;
  host_state.substitute_with = 0;  // DRM_FORMAT_MOD_LINEAR
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  constexpr uint64_t kTiled = 0x0700000000000006ULL;  // BROADCOM_UIF
  IhsFormatModifier want{};
  want.fourcc = 0x34325241;  // AR24
  want.modifier = kTiled;
  IhsPvRequirements req = make_req(IHS_PV_KIND_DRM_PLANE);
  req.formats = &want;
  req.format_count = 1;

  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);

  // The host saw what was negotiated, and the caller is told what it granted.
  EXPECT_EQ(host_state.last_grant_modifier_in, kTiled);
  EXPECT_EQ(grant.format.modifier, 0ULL);
  EXPECT_EQ(grant.format.fourcc, want.fourcc);

  detach_host();
}

// A host with no opinion leaves the negotiated modifier alone.
TEST(IhsPvSurface, GrantLeavesTheModifierAloneByDefault) {
  MockHost host_state;
  host_state.caps_kinds = IHS_PV_KIND_DRM_PLANE;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  constexpr uint64_t kTiled = 0x0700000000000006ULL;
  IhsFormatModifier want{};
  want.fourcc = 0x34325241;
  want.modifier = kTiled;
  IhsPvRequirements req = make_req(IHS_PV_KIND_DRM_PLANE);
  req.formats = &want;
  req.format_count = 1;

  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.format.modifier, kTiled);

  detach_host();
}

// Only the floor is common -> SOFTWARE_SHM, and the accessor forwards its fd.
TEST(IhsPvSurface, NegotiateFallsToFloor) {
  MockHost host_state;
  host_state.caps_kinds = IHS_PV_KIND_SOFTWARE_SHM;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req =
      make_req(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM);
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant), IHS_PV_OK);
  EXPECT_EQ(grant.granted_kind, IHS_PV_KIND_SOFTWARE_SHM);

  size_t stride = 0;
  EXPECT_EQ(ihs_pv_grant_shm_fd(fake_view(), &stride), 7);
  EXPECT_EQ(stride, 256u);

  detach_host();
}

// No overlap between requirement and backend -> UNSUPPORTED, host never
// granted.
TEST(IhsPvSurface, NegotiateUnsupportedWhenNoOverlap) {
  MockHost host_state;
  host_state.caps_kinds = IHS_PV_KIND_SOFTWARE_SHM;  // no DRM plane
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsPvRequirements req = make_req(IHS_PV_KIND_DRM_PLANE);
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  EXPECT_EQ(ihs_pv_negotiate(fake_view(), &req, &grant),
            IHS_PV_ERR_UNSUPPORTED);
  EXPECT_EQ(host_state.grant_calls, 0);

  detach_host();
}

// submit forwards the view + frame to the host present path.
TEST(IhsPvSurface, SubmitForwardsToHost) {
  MockHost host_state;
  const IhsPvHost host = make_host(&host_state);
  ihs_pv_set_host(&host);

  IhsFrame frame{};
  frame.struct_size = sizeof(frame);
  frame.width = 64;
  frame.height = 64;
  frame.plane_count = 1;
  int release_fd = 0;
  EXPECT_EQ(ihs_pv_submit(fake_view(), &frame, -1, &release_fd), IHS_PV_OK);
  EXPECT_EQ(host_state.submit_calls, 1);
  EXPECT_EQ(host_state.last_submit_view, fake_view());
  EXPECT_EQ(release_fd, -1);

  detach_host();
}

// ---- platform-thread tasks --------------------------------------------------

namespace {

// Stands in for the shell's platform runner: queues rather than runs, so a
// test can prove the task was not run inline.
struct MockRunner {
  std::vector<std::pair<IhsPvTaskFn, void*>> queued;
  int on_platform_thread = 0;
  int post_rc = IHS_PV_OK;
};

int mock_post(void* u, IhsPvTaskFn fn, void* task_user_data) {
  auto* r = static_cast<MockRunner*>(u);
  if (r->post_rc == IHS_PV_OK) {
    r->queued.emplace_back(fn, task_user_data);
  }
  return r->post_rc;
}

int mock_is_platform(void* u) {
  return static_cast<MockRunner*>(u)->on_platform_thread;
}

IhsPvHost make_task_host(MockRunner* r) {
  IhsPvHost h{};
  h.struct_size = sizeof(h);
  h.user_data = r;
  h.post_platform_task = mock_post;
  h.is_platform_thread = mock_is_platform;
  return h;
}

void count_task(void* u) {
  ++*static_cast<int*>(u);
}

}  // namespace

TEST(IhsPvSurface, PostPlatformTaskForwardsWithoutRunningInline) {
  MockRunner runner;
  const IhsPvHost host = make_task_host(&runner);
  ihs_pv_set_host(&host);

  int ran = 0;
  EXPECT_EQ(ihs_pv_post_platform_task(count_task, &ran), IHS_PV_OK);
  EXPECT_EQ(ran, 0);  // queued, not called
  ASSERT_EQ(runner.queued.size(), 1u);
  runner.queued[0].first(runner.queued[0].second);
  EXPECT_EQ(ran, 1);

  // The host's refusal (no running engine) is passed through.
  runner.post_rc = IHS_PV_ERR_NO_BACKEND;
  EXPECT_EQ(ihs_pv_post_platform_task(count_task, &ran), IHS_PV_ERR_NO_BACKEND);
  EXPECT_EQ(runner.queued.size(), 1u);

  detach_host();
}

TEST(IhsPvSurface, PostPlatformTaskRejectsNullAndHeadless) {
  detach_host();
  int ran = 0;
  EXPECT_EQ(ihs_pv_post_platform_task(count_task, &ran),
            IHS_PV_ERR_NO_REGISTRY);
  EXPECT_EQ(ihs_pv_is_platform_thread(), 0);

  MockRunner runner;
  const IhsPvHost host = make_task_host(&runner);
  ihs_pv_set_host(&host);
  EXPECT_EQ(ihs_pv_post_platform_task(nullptr, &ran), IHS_PV_ERR_INVALID);
  EXPECT_TRUE(runner.queued.empty());
  detach_host();
}

TEST(IhsPvSurface, IsPlatformThreadForwardsToHost) {
  MockRunner runner;
  const IhsPvHost host = make_task_host(&runner);
  ihs_pv_set_host(&host);

  EXPECT_EQ(ihs_pv_is_platform_thread(), 0);
  runner.on_platform_thread = 1;
  EXPECT_EQ(ihs_pv_is_platform_thread(), 1);
  runner.on_platform_thread = 7;  // any non-zero is normalized to 1
  EXPECT_EQ(ihs_pv_is_platform_thread(), 1);

  detach_host();
}

// A shell built against the header before these members existed passes a
// shorter IhsPvHost. The forwarders must not read past its end.
TEST(IhsPvSurface, PlatformTaskAbsentOnOlderHost) {
  MockRunner runner;
  IhsPvHost host = make_task_host(&runner);
  host.struct_size = offsetof(IhsPvHost, post_platform_task);
  ihs_pv_set_host(&host);

  int ran = 0;
  EXPECT_EQ(ihs_pv_post_platform_task(count_task, &ran),
            IHS_PV_ERR_NO_REGISTRY);
  EXPECT_EQ(ihs_pv_is_platform_thread(), 0);
  EXPECT_TRUE(runner.queued.empty());

  detach_host();
}

TEST(IhsPvSurface, SubTableCarriesPlatformTaskEntryPoints) {
  const IhsApi* api = ihs_get_api(IHS_SHARED_ABI_VERSION);
  ASSERT_NE(api, nullptr);
  const IhsPlatformViewApi* pv = api->platform_view;
  ASSERT_NE(pv, nullptr);
  ASSERT_GE(pv->struct_size, offsetof(IhsPlatformViewApi, is_platform_thread) +
                                 sizeof(pv->is_platform_thread));
  EXPECT_EQ(pv->post_platform_task, &ihs_pv_post_platform_task);
  EXPECT_EQ(pv->is_platform_thread, &ihs_pv_is_platform_thread);
}

// ---- buffer retirement ------------------------------------------------------

namespace {

struct RetireRecorder {
  int calls = 0;
  IhsPlatformView* last_view = nullptr;
  uint32_t last_id = 0;
};

int mock_retire(void* u, IhsPlatformView* view, uint32_t buffer_id) {
  auto* r = static_cast<RetireRecorder*>(u);
  ++r->calls;
  r->last_view = view;
  r->last_id = buffer_id;
  return IHS_PV_OK;
}

}  // namespace

TEST(IhsPvSurface, RetireBufferForwardsToHost) {
  RetireRecorder rec;
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.user_data = &rec;
  host.retire_buffer = mock_retire;
  ihs_pv_set_host(&host);

  EXPECT_EQ(ihs_pv_retire_buffer(fake_view(), 41), IHS_PV_OK);
  EXPECT_EQ(rec.calls, 1);
  EXPECT_EQ(rec.last_view, fake_view());
  EXPECT_EQ(rec.last_id, 41u);

  EXPECT_EQ(ihs_pv_retire_buffer(nullptr, 41), IHS_PV_ERR_INVALID);
  EXPECT_EQ(rec.calls, 1);

  detach_host();
  EXPECT_EQ(ihs_pv_retire_buffer(fake_view(), 41), IHS_PV_ERR_NO_REGISTRY);
}

// A shell whose IhsPvHost ends before retire_buffer (built against 1.8 or
// earlier) cannot drop imports; the caller must hear that rather than OK.
TEST(IhsPvSurface, RetireBufferAbsentOnOlderHost) {
  RetireRecorder rec;
  IhsPvHost host{};
  host.struct_size = offsetof(IhsPvHost, retire_buffer);
  host.user_data = &rec;
  host.retire_buffer = mock_retire;
  ihs_pv_set_host(&host);

  EXPECT_EQ(ihs_pv_retire_buffer(fake_view(), 41), IHS_PV_ERR_NO_BACKEND);
  EXPECT_EQ(rec.calls, 0);

  detach_host();
}

TEST(IhsPvSurface, SubTableCarriesRetireBuffer) {
  const IhsApi* api = ihs_get_api(IHS_SHARED_ABI_VERSION);
  ASSERT_NE(api, nullptr);
  const IhsPlatformViewApi* pv = api->platform_view;
  ASSERT_NE(pv, nullptr);
  ASSERT_GE(pv->struct_size, offsetof(IhsPlatformViewApi, retire_buffer) +
                                 sizeof(pv->retire_buffer));
  EXPECT_EQ(pv->retire_buffer, &ihs_pv_retire_buffer);
}

// ---- layer lists
// -------------------------------------------------------------

namespace {

struct LayersRecorder {
  int calls = 0;
  size_t count = 0;
  uint64_t seq = 0;
  uint32_t first_layer_id = 0;
  bool out_prefilled = false;  // every out slot was -1 on arrival
};

int mock_submit_layers(
    void* u,
    IhsPlatformView* /*view*/,
    const IhsLayer* layers,
    size_t layer_count,
    uint64_t seq,
    // Non-const: the IhsPvHost::submit_layers signature.
    int* out_release_fence_fds) {  // NOLINT(readability-non-const-parameter)
  auto* r = static_cast<LayersRecorder*>(u);
  ++r->calls;
  r->count = layer_count;
  r->seq = seq;
  r->first_layer_id = layer_count > 0 ? layers[0].layer_id : 0;
  r->out_prefilled = true;
  for (size_t i = 0; out_release_fence_fds != nullptr && i < layer_count; ++i) {
    r->out_prefilled = r->out_prefilled && out_release_fence_fds[i] == -1;
  }
  return IHS_PV_OK;
}

IhsFrame LayerFrame(int fd) {
  IhsFrame f{};
  f.struct_size = sizeof(f);
  f.width = 16;
  f.height = 16;
  f.plane_count = 1;
  f.plane_fd[0] = fd;
  f.buffer_id = 1;
  return f;
}

IhsLayer MakeLayer(const IhsFrame* frame, uint32_t id) {
  IhsLayer l{};
  l.struct_size = sizeof(l);
  l.frame = frame;
  l.acquire_fence_fd = -1;
  l.layer_id = id;
  return l;
}

// An fd the test can tell was closed: one end of a pipe.
int OpenFd() {
  int p[2] = {-1, -1};
  EXPECT_EQ(pipe(p), 0);
  close(p[1]);
  return p[0];
}

bool IsOpen(int fd) {
  return fcntl(fd, F_GETFD) != -1;
}

}  // namespace

TEST(IhsPvSurface, SubmitLayersForwardsToHost) {
  LayersRecorder rec;
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.user_data = &rec;
  host.submit_layers = mock_submit_layers;
  ihs_pv_set_host(&host);

  const IhsFrame f0 = LayerFrame(-1);
  const IhsFrame f1 = LayerFrame(-1);
  const IhsLayer layers[2] = {MakeLayer(&f0, 5), MakeLayer(&f1, 9)};
  int out[2] = {123, 456};
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), layers, 2, 77, out), IHS_PV_OK);
  EXPECT_EQ(rec.calls, 1);
  EXPECT_EQ(rec.count, 2u);
  EXPECT_EQ(rec.seq, 77u);
  EXPECT_EQ(rec.first_layer_id, 5u);
  EXPECT_TRUE(rec.out_prefilled);

  // An empty list is a valid submit: the view shows nothing.
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), nullptr, 0, 0, nullptr),
            IHS_PV_OK);
  EXPECT_EQ(rec.count, 0u);

  detach_host();
}

// A list that cannot be trusted is refused whole, and -- the one exception to
// the ownership rule -- none of its fds are closed.
TEST(IhsPvSurface, SubmitLayersRejectsMalformedListsAndClosesNothing) {
  LayersRecorder rec;
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.user_data = &rec;
  host.submit_layers = mock_submit_layers;
  ihs_pv_set_host(&host);

  const int fd = OpenFd();
  const IhsFrame f = LayerFrame(fd);
  IhsLayer layer = MakeLayer(&f, 1);

  EXPECT_EQ(ihs_pv_submit_layers(nullptr, &layer, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), nullptr, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);
  std::vector<IhsLayer> too_many(IHS_PV_MAX_LAYERS + 1, layer);
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), too_many.data(), too_many.size(),
                                 0, nullptr),
            IHS_PV_ERR_INVALID);
  IhsLayer short_layer = layer;
  short_layer.struct_size = 8;
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &short_layer, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);
  IhsLayer no_frame = layer;
  no_frame.frame = nullptr;
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &no_frame, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);
  // A frame that ends before buffer_id: layers are keyed by it.
  IhsFrame old_frame = f;
  old_frame.struct_size = offsetof(IhsFrame, buffer_id);
  IhsLayer old_layer = MakeLayer(&old_frame, 1);
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &old_layer, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);

  EXPECT_EQ(rec.calls, 0);
  EXPECT_TRUE(IsOpen(fd));
  close(fd);
  detach_host();
}

// An image layer (1.16) shows an EGLImage instead of a frame; a list may mix
// the two.
TEST(IhsPvSurface, SubmitLayersForwardsImageLayers) {
  LayersRecorder rec;
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.user_data = &rec;
  host.submit_layers = mock_submit_layers;
  ihs_pv_set_host(&host);

  int token = 0;  // stands in for an EGLImage: the registry never touches it
  IhsImage image{};
  image.struct_size = sizeof(image);
  image.egl_image = &token;
  image.width = 16;
  image.height = 16;
  image.buffer_id = 3;
  IhsLayer image_layer = MakeLayer(nullptr, 4);
  image_layer.image = &image;
  const IhsFrame f = LayerFrame(-1);
  const IhsLayer layers[2] = {image_layer, MakeLayer(&f, 5)};
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), layers, 2, 1, nullptr),
            IHS_PV_OK);
  EXPECT_EQ(rec.calls, 1);
  EXPECT_EQ(rec.count, 2u);
  EXPECT_EQ(rec.first_layer_id, 4u);
  detach_host();
}

// A layer carries a frame or an image, never both, and an image must be one.
TEST(IhsPvSurface, SubmitLayersRejectsMalformedImageLayersAndClosesNothing) {
  LayersRecorder rec;
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.user_data = &rec;
  host.submit_layers = mock_submit_layers;
  ihs_pv_set_host(&host);

  const int fd = OpenFd();
  const IhsFrame f = LayerFrame(fd);
  int token = 0;
  IhsImage image{};
  image.struct_size = sizeof(image);
  image.egl_image = &token;

  IhsLayer both = MakeLayer(&f, 1);
  both.image = &image;
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &both, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);
  IhsImage short_image = image;
  short_image.struct_size = 8;
  IhsLayer bad = MakeLayer(nullptr, 1);
  bad.image = &short_image;
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &bad, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);
  IhsImage no_image = image;
  no_image.egl_image = nullptr;
  bad.image = &no_image;
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &bad, 1, 0, nullptr),
            IHS_PV_ERR_INVALID);

  EXPECT_EQ(rec.calls, 0);
  EXPECT_TRUE(IsOpen(fd));
  close(fd);
  detach_host();
}

// A layer from a header before 1.16 ends before @image: whatever follows it in
// memory is not read as one.
TEST(IhsPvSurface, SubmitLayersIgnoresImageBeyondAnOlderLayer) {
  LayersRecorder rec;
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.user_data = &rec;
  host.submit_layers = mock_submit_layers;
  ihs_pv_set_host(&host);

  const IhsFrame f = LayerFrame(-1);
  IhsLayer old = MakeLayer(&f, 1);
  old.struct_size = offsetof(IhsLayer, image);
  int token = 0;
  IhsImage image{};
  image.struct_size = sizeof(image);
  image.egl_image = &token;
  old.image = &image;  // past its struct_size: must be ignored
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &old, 1, 0, nullptr), IHS_PV_OK);
  EXPECT_EQ(rec.calls, 1);
  detach_host();
}

// With nowhere to send the list, its fds are still consumed.
TEST(IhsPvSurface, SubmitLayersConsumesFdsWithNoHost) {
  detach_host();
  const int plane = OpenFd();
  const int fence = OpenFd();
  const IhsFrame f = LayerFrame(plane);
  IhsLayer layer = MakeLayer(&f, 1);
  layer.acquire_fence_fd = fence;
  int out = 99;
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &layer, 1, 0, &out),
            IHS_PV_ERR_NO_REGISTRY);
  EXPECT_EQ(out, -1);
  EXPECT_FALSE(IsOpen(plane));
  EXPECT_FALSE(IsOpen(fence));
}

// A shell whose IhsPvHost ends before submit_layers: the list cannot be shown,
// and the caller hears so; its fds are consumed all the same.
TEST(IhsPvSurface, SubmitLayersAbsentOnOlderHost) {
  LayersRecorder rec;
  IhsPvHost host{};
  host.struct_size = offsetof(IhsPvHost, submit_layers);
  host.user_data = &rec;
  host.submit_layers = mock_submit_layers;
  ihs_pv_set_host(&host);

  const int plane = OpenFd();
  const IhsFrame f = LayerFrame(plane);
  const IhsLayer layer = MakeLayer(&f, 1);
  EXPECT_EQ(ihs_pv_submit_layers(fake_view(), &layer, 1, 0, nullptr),
            IHS_PV_ERR_NO_BACKEND);
  EXPECT_EQ(rec.calls, 0);
  EXPECT_FALSE(IsOpen(plane));
  detach_host();
}

TEST(IhsPvSurface, SubTableCarriesSubmitLayers) {
  const IhsApi* api = ihs_get_api(IHS_SHARED_ABI_VERSION);
  ASSERT_NE(api, nullptr);
  const IhsPlatformViewApi* pv = api->platform_view;
  ASSERT_NE(pv, nullptr);
  ASSERT_GE(pv->struct_size, offsetof(IhsPlatformViewApi, submit_layers) +
                                 sizeof(pv->submit_layers));
  EXPECT_EQ(pv->submit_layers, &ihs_pv_submit_layers);
}
