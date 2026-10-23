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

// A compositor's close (xdg_toplevel.close -- the title bar's X, the window
// menu's Close) ends the shell.
//
// It used to set a WaylandWindow member nothing read, so the window stayed up
// and only a signal could stop the process. A hand-rolled libwayland server
// stands in for the compositor: it gives the surface its xdg role and sends
// close the moment the toplevel exists.

#include "main_loop_waker.h"
#include "shutdown_flag.h"
#include "wayland/shell/xdg_shell.h"
#include "wayland/window.h"

extern "C" {
#include <poll.h>
#include <sys/socket.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
}

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

namespace {

namespace xc = xdg_shell::client;

// ---- Server: the compositor --------------------------------------------

struct Server {
  bool send_close = true;
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

// The xdg objects take a dispatcher rather than an implementation struct:
// only destroy and the two role requests matter, and xdg_toplevel alone has
// fourteen.
int ToplevelDispatch(const void* /*impl*/,
                     void* target,
                     uint32_t opcode,
                     const wl_message* /*msg*/,
                     wl_argument* /*args*/) {
  if (opcode == xc::xdg_toplevel_traits::Op::Destroy) {
    wl_resource_destroy(static_cast<wl_resource*>(target));
  }
  return 0;
}

int XdgSurfaceDispatch(const void* /*impl*/,
                       void* target,
                       uint32_t opcode,
                       const wl_message* /*msg*/,
                       wl_argument* args) {
  auto* r = static_cast<wl_resource*>(target);
  if (opcode == xc::xdg_surface_traits::Op::Destroy) {
    wl_resource_destroy(r);
  } else if (opcode == xc::xdg_surface_traits::Op::GetToplevel) {
    wl_resource* t = wl_resource_create(
        wl_resource_get_client(r), &xc::xdg_toplevel_traits::wl_iface(),
        wl_resource_get_version(r), static_cast<int>(args[0].n));
    wl_resource_set_dispatcher(t, &ToplevelDispatch, nullptr, nullptr, nullptr);
    const auto* srv = static_cast<Server*>(wl_resource_get_user_data(r));
    if (srv->send_close) {
      wl_resource_post_event(t, xc::xdg_toplevel_traits::Evt::Close);
    }
  }
  return 0;
}

int WmBaseDispatch(const void* /*impl*/,
                   void* target,
                   uint32_t opcode,
                   const wl_message* /*msg*/,
                   wl_argument* args) {
  auto* r = static_cast<wl_resource*>(target);
  if (opcode == xc::xdg_wm_base_traits::Op::Destroy) {
    wl_resource_destroy(r);
  } else if (opcode == xc::xdg_wm_base_traits::Op::GetXdgSurface) {
    wl_resource* s = wl_resource_create(
        wl_resource_get_client(r), &xc::xdg_surface_traits::wl_iface(),
        wl_resource_get_version(r), static_cast<int>(args[0].n));
    wl_resource_set_dispatcher(s, &XdgSurfaceDispatch, nullptr,
                               wl_resource_get_user_data(r), nullptr);
  }
  return 0;
}

void WmBaseBind(wl_client* c, void* data, uint32_t version, uint32_t id) {
  wl_resource* r =
      wl_resource_create(c, &xc::xdg_wm_base_traits::wl_iface(),
                         static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_dispatcher(r, &WmBaseDispatch, nullptr, data, nullptr);
}

// ---- Client: what WaylandWindow does -------------------------------------

struct Globals {
  ivi::XdgShell* shell{};
  uint32_t compositor = 0;
};

void OnGlobal(void* data,
              wl_registry* registry,
              uint32_t name,
              const char* interface,
              uint32_t version) {
  auto* g = static_cast<Globals*>(data);
  if (std::string_view(interface) == "wl_compositor") {
    g->compositor = name;
  } else {
    (void)g->shell->TryBindGlobal(registry, name, interface, version);
  }
}
void OnGlobalRemove(void* /*data*/, wl_registry* /*r*/, uint32_t /*name*/) {}
constexpr wl_registry_listener kRegistryListener{&OnGlobal, &OnGlobalRemove};

bool WakerSignaled() {
  pollfd p{MainLoopWaker::instance().fd(), POLLIN, 0};
  return poll(&p, 1, 0) == 1 && (p.revents & POLLIN) != 0;
}

void DrainWaker() {
  // Wait() drains pending wake tokens before returning.
  MainLoopWaker::instance().Wait(0);
}

// Gives a surface its xdg role against a compositor that does or does not
// send close, with the window's own close handler installed. Returns whether
// that handler ran.
bool RunAgainst(bool send_close) {
  bool closed = false;
  std::array<int, 2> sv{-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  Server srv{send_close};
  wl_display* server = wl_display_create();
  EXPECT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, &wl_compositor_interface, 4, nullptr,
                   &CompositorBind);
  wl_global_create(server, &xc::xdg_wm_base_traits::wl_iface(), 1, &srv,
                   &WmBaseBind);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  EXPECT_NE(client, nullptr);
  if (client != nullptr) {
    {
      ivi::XdgShell shell;
      Globals g{&shell};
      wl_registry* registry = wl_display_get_registry(client);
      wl_registry_add_listener(registry, &kRegistryListener, &g);
      EXPECT_NE(wl_display_roundtrip(client), -1);
      EXPECT_TRUE(shell.IsBound());

      auto* compositor = static_cast<wl_compositor*>(wl_registry_bind(
          registry, g.compositor, &wl_compositor_interface, 4));
      wl_surface* surface = wl_compositor_create_surface(compositor);

      ivi::WindowConfig cfg;
      cfg.app_id = "toplevel_close";
      cfg.on_close = [&closed] {
        closed = true;
        WaylandWindow::HandleCompositorClose(0);
      };
      {
        const auto role = shell.CreateSurface(surface, cfg);
        EXPECT_NE(wl_display_roundtrip(client), -1);
      }
      wl_surface_destroy(surface);
      wl_compositor_destroy(compositor);
      wl_registry_destroy(registry);
      wl_display_roundtrip(client);
    }
    wl_display_disconnect(client);
  }
  wl_display_terminate(server);
  server_thread.join();
  wl_display_destroy_clients(server);
  wl_display_destroy(server);
  return closed;
}

class ToplevelClose : public ::testing::Test {
 protected:
  void SetUp() override {
    // SignalWake writes an eventfd that instance() creates.
    (void)MainLoopWaker::instance();
    DrainWaker();
    running = 1;
  }
  void TearDown() override {
    DrainWaker();
    running = 1;
  }
};

}  // namespace

TEST_F(ToplevelClose, EndsTheShell) {
  ASSERT_TRUE(RunAgainst(/*send_close=*/true))
      << "xdg_toplevel.close never reached WindowConfig::on_close";
  EXPECT_EQ(running, 0) << "the run flag is still set; the loop keeps going";
  EXPECT_TRUE(WakerSignaled())
      << "the loop was not woken, so an idle shell would not notice";
}

// Without the event nothing changes: what EndsTheShell sees is the close, not
// a side effect of creating and tearing down the role.
TEST_F(ToplevelClose, NothingHappensWithoutIt) {
  EXPECT_FALSE(RunAgainst(/*send_close=*/false));
  EXPECT_EQ(running, 1);
  EXPECT_FALSE(WakerSignaled());
}
