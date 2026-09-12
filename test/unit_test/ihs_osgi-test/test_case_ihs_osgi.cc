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
 * Unit tests for the ihs_shared OSGi handshake surface (ihs/ihs_osgi.h and the
 * shell-only ihs/ihs_osgi_host.h).
 *
 * libihs_shared holds no OSGi state: every entry point validates its arguments,
 * reads one atomic pointer, and calls through it. So what is testable here is
 * exactly that -- argument validation, the safe defaults with no host, a host
 * table too short to call through, and that a real table is forwarded to with
 * the arguments unchanged. The registry behaviour behind the table is the
 * shell's, and is covered by osgi_bridge-test.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ihs/ihs_osgi.h"
#include "ihs/ihs_osgi_host.h"

namespace {

// A stand-in for the shell-owned opaque handle. libihs_shared only ever passes
// it through, so any distinguishable address will do.
int g_bundle_object = 0;
IhsOsgiBundle* FakeHandle() {
  return reinterpret_cast<IhsOsgiBundle*>(&g_bundle_object);
}

// Records what the forwarders passed through.
struct FakeHost {
  static inline int framework_calls = 0;
  static inline int bundle_calls = 0;
  static inline int active_calls = 0;
  static inline int stopped_calls = 0;
  static inline int unregister_calls = 0;
  static inline std::string last_name;
  static inline int64_t last_port = 0;
  static inline void* last_dl_data = nullptr;
  static inline void* last_user_data = nullptr;
  static inline IhsOsgiBundle* last_handle = nullptr;

  static void Reset() {
    framework_calls = 0;
    bundle_calls = 0;
    active_calls = 0;
    stopped_calls = 0;
    unregister_calls = 0;
    last_name.clear();
    last_port = 0;
    last_dl_data = nullptr;
    last_user_data = nullptr;
    last_handle = nullptr;
  }

  static int RegisterFramework(void* user_data,
                               void* dart_api_dl_data,
                               int64_t port,
                               size_t* out_served) {
    ++framework_calls;
    last_user_data = user_data;
    last_dl_data = dart_api_dl_data;
    last_port = port;
    if (out_served != nullptr) {
      *out_served = 3;
    }
    return IHS_OSGI_OK;
  }

  static int RegisterBundle(void* user_data,
                            void* dart_api_dl_data,
                            int64_t port,
                            const char* symbolic_name,
                            IhsOsgiBundle** out_bundle) {
    ++bundle_calls;
    last_user_data = user_data;
    last_dl_data = dart_api_dl_data;
    last_port = port;
    last_name = symbolic_name != nullptr ? symbolic_name : "";
    *out_bundle = FakeHandle();
    return IHS_OSGI_OK;
  }

  static int ReportActive(void* user_data, IhsOsgiBundle* bundle) {
    ++active_calls;
    last_user_data = user_data;
    last_handle = bundle;
    return IHS_OSGI_OK;
  }

  static int ReportStopped(void* user_data, IhsOsgiBundle* bundle) {
    ++stopped_calls;
    last_user_data = user_data;
    last_handle = bundle;
    return IHS_OSGI_OK;
  }

  static int Unregister(void* user_data, IhsOsgiBundle* bundle) {
    ++unregister_calls;
    last_user_data = user_data;
    last_handle = bundle;
    return IHS_OSGI_OK;
  }
};

int g_user_data = 0;

IhsOsgiHost MakeHost() {
  IhsOsgiHost host{};
  host.struct_size = sizeof(IhsOsgiHost);
  host.user_data = &g_user_data;
  host.register_framework = &FakeHost::RegisterFramework;
  host.register_bundle = &FakeHost::RegisterBundle;
  host.report_active = &FakeHost::ReportActive;
  host.report_stopped = &FakeHost::ReportStopped;
  host.unregister = &FakeHost::Unregister;
  return host;
}

IhsOsgiPeerInfo MakePeer() {
  IhsOsgiPeerInfo peer{};
  peer.struct_size = sizeof(IhsOsgiPeerInfo);
  peer.dart_api_dl_data = &g_user_data;
  peer.port = 4242;
  return peer;
}

IhsOsgiBundleInfo MakeBundleInfo(const char* name) {
  IhsOsgiBundleInfo info{};
  info.struct_size = sizeof(IhsOsgiBundleInfo);
  info.peer = MakePeer();
  info.symbolic_name = name;
  return info;
}

// The host is process-global, so every test installs and removes its own.
class IhsOsgiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    FakeHost::Reset();
    ihs_osgi_set_host(nullptr);
  }
  void TearDown() override { ihs_osgi_set_host(nullptr); }
};

}  // namespace

// --- Safe defaults ----------------------------------------------------------
//
// A bundle running under a shell that does not do OSGi is a configuration
// mistake, not a crash. Every call has to answer for itself rather than assume
// a host is there.

TEST_F(IhsOsgiTest, WithoutAHostEverythingIsUnavailable) {
  EXPECT_FALSE(ihs_osgi_available());

  const IhsOsgiPeerInfo peer = MakePeer();
  EXPECT_EQ(ihs_osgi_register_framework(&peer, nullptr),
            IHS_OSGI_ERR_UNAVAILABLE);

  const IhsOsgiBundleInfo info = MakeBundleInfo("com.ivi.cluster");
  IhsOsgiBundle* bundle = FakeHandle();
  EXPECT_EQ(ihs_osgi_register_bundle(&info, &bundle), IHS_OSGI_ERR_UNAVAILABLE);
  EXPECT_EQ(bundle, nullptr) << "out_bundle is cleared before the host check";

  EXPECT_EQ(ihs_osgi_report_active(FakeHandle()), IHS_OSGI_ERR_UNAVAILABLE);
  EXPECT_EQ(ihs_osgi_report_stopped(FakeHandle()), IHS_OSGI_ERR_UNAVAILABLE);
  EXPECT_EQ(ihs_osgi_unregister(FakeHandle()), IHS_OSGI_ERR_UNAVAILABLE);
}

TEST_F(IhsOsgiTest, UninstallingTheHostRestoresTheSafeDefaults) {
  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);
  ASSERT_TRUE(ihs_osgi_available());

  // Teardown: the shell is going away, and a call must not follow it there.
  ihs_osgi_set_host(nullptr);
  EXPECT_FALSE(ihs_osgi_available());
  EXPECT_EQ(ihs_osgi_report_active(FakeHandle()), IHS_OSGI_ERR_UNAVAILABLE);
  EXPECT_EQ(FakeHost::active_calls, 0);
}

// A table shorter than this build's definition means an older shell against a
// newer ihs_shared: the trailing entries are absent rather than NULL, so
// calling through them would be undefined.
TEST_F(IhsOsgiTest, AShortHostTableIsRefusedOutright) {
  IhsOsgiHost host = MakeHost();
  host.struct_size = sizeof(IhsOsgiHost) - 1;
  ihs_osgi_set_host(&host);

  EXPECT_FALSE(ihs_osgi_available());
  EXPECT_EQ(ihs_osgi_report_active(FakeHandle()), IHS_OSGI_ERR_UNAVAILABLE);
  EXPECT_EQ(FakeHost::active_calls, 0);
}

// A NULL entry means the capability is absent, which is reported rather than
// pretended.
TEST_F(IhsOsgiTest, ANullEntryReportsUnavailableRatherThanCrashing) {
  IhsOsgiHost host = MakeHost();
  host.report_active = nullptr;
  ihs_osgi_set_host(&host);

  ASSERT_TRUE(ihs_osgi_available());
  EXPECT_EQ(ihs_osgi_report_active(FakeHandle()), IHS_OSGI_ERR_UNAVAILABLE);
  // The rest of the table still works.
  EXPECT_EQ(ihs_osgi_report_stopped(FakeHandle()), IHS_OSGI_OK);
}

// --- Argument validation ----------------------------------------------------
//
// Checked before the host is consulted, so the answer is the same with and
// without one.

TEST_F(IhsOsgiTest, RejectsMalformedArgumentsBeforeReachingTheHost) {
  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);

  EXPECT_EQ(ihs_osgi_register_framework(nullptr, nullptr),
            IHS_OSGI_ERR_INVALID);

  IhsOsgiPeerInfo short_peer = MakePeer();
  short_peer.struct_size = sizeof(IhsOsgiPeerInfo) - 1;
  EXPECT_EQ(ihs_osgi_register_framework(&short_peer, nullptr),
            IHS_OSGI_ERR_INVALID);

  // A closed or never-opened port would look like a successful handshake and
  // then silently deliver nothing.
  IhsOsgiPeerInfo zero_port = MakePeer();
  zero_port.port = 0;
  EXPECT_EQ(ihs_osgi_register_framework(&zero_port, nullptr),
            IHS_OSGI_ERR_INVALID);

  EXPECT_EQ(FakeHost::framework_calls, 0);
}

TEST_F(IhsOsgiTest, RejectsABundleWithNoNameOrNoOutParameter) {
  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);

  IhsOsgiBundle* bundle = nullptr;
  EXPECT_EQ(ihs_osgi_register_bundle(nullptr, &bundle), IHS_OSGI_ERR_INVALID);

  IhsOsgiBundleInfo no_name = MakeBundleInfo(nullptr);
  EXPECT_EQ(ihs_osgi_register_bundle(&no_name, &bundle), IHS_OSGI_ERR_INVALID);

  IhsOsgiBundleInfo empty_name = MakeBundleInfo("");
  EXPECT_EQ(ihs_osgi_register_bundle(&empty_name, &bundle),
            IHS_OSGI_ERR_INVALID);

  const IhsOsgiBundleInfo ok = MakeBundleInfo("com.ivi.cluster");
  EXPECT_EQ(ihs_osgi_register_bundle(&ok, nullptr), IHS_OSGI_ERR_INVALID);

  EXPECT_EQ(FakeHost::bundle_calls, 0);
}

TEST_F(IhsOsgiTest, AReportNeedsAHandle) {
  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);

  EXPECT_EQ(ihs_osgi_report_active(nullptr), IHS_OSGI_ERR_INVALID);
  EXPECT_EQ(ihs_osgi_report_stopped(nullptr), IHS_OSGI_ERR_INVALID);
  EXPECT_EQ(FakeHost::active_calls, 0);
  EXPECT_EQ(FakeHost::stopped_calls, 0);
}

// Unregister is the exception, and deliberately so: a teardown path running
// after a failed registration has nothing to release and needs no special case.
TEST_F(IhsOsgiTest, UnregisteringANullHandleSucceedsWithOrWithoutAHost) {
  EXPECT_EQ(ihs_osgi_unregister(nullptr), IHS_OSGI_OK);

  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);
  EXPECT_EQ(ihs_osgi_unregister(nullptr), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::unregister_calls, 0) << "nothing to release";
}

// --- Forwarding -------------------------------------------------------------

TEST_F(IhsOsgiTest, ForwardsEveryArgumentToTheHostUnchanged) {
  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);

  const IhsOsgiPeerInfo peer = MakePeer();
  size_t served = 0;
  EXPECT_EQ(ihs_osgi_register_framework(&peer, &served), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::framework_calls, 1);
  EXPECT_EQ(FakeHost::last_port, 4242);
  EXPECT_EQ(FakeHost::last_dl_data, &g_user_data);
  EXPECT_EQ(FakeHost::last_user_data, &g_user_data);
  EXPECT_EQ(served, 3u) << "out_served is written through";

  const IhsOsgiBundleInfo info = MakeBundleInfo("com.ivi.cluster");
  IhsOsgiBundle* bundle = nullptr;
  EXPECT_EQ(ihs_osgi_register_bundle(&info, &bundle), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::bundle_calls, 1);
  EXPECT_EQ(FakeHost::last_name, "com.ivi.cluster");
  EXPECT_EQ(bundle, FakeHandle()) << "the shell mints the handle, not this lib";

  EXPECT_EQ(ihs_osgi_report_active(bundle), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::last_handle, bundle);
  EXPECT_EQ(ihs_osgi_report_stopped(bundle), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::last_handle, bundle);
  EXPECT_EQ(ihs_osgi_unregister(bundle), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::unregister_calls, 1);
}

// out_served is a diagnostic; a caller with no interest passes NULL and the
// forwarder must not invent a place to write it.
TEST_F(IhsOsgiTest, ANullOutServedIsAccepted) {
  const IhsOsgiHost host = MakeHost();
  ihs_osgi_set_host(&host);

  const IhsOsgiPeerInfo peer = MakePeer();
  EXPECT_EQ(ihs_osgi_register_framework(&peer, nullptr), IHS_OSGI_OK);
  EXPECT_EQ(FakeHost::framework_calls, 1);
}
