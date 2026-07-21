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
               const IhsFormatModifier* /*fmt*/,
               uint32_t* out_plane,
               int* out_shm_fd,
               size_t* out_shm_stride) {
  auto* m = static_cast<MockHost*>(u);
  ++m->grant_calls;
  m->last_grant_kind = kind;
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
