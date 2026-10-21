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

// The GL-composited platform-view release path, on a real compositor.
//
// #530: a platform view that the compositor draws through a texture never
// reaches a plane, so the scanout retire that fires OnScanoutRelease never
// happens for it. Three separate things had to be right before a producer got
// its buffer back, and each hid the next -- the frame has to be queued at the
// composite site, the queue has to be drained on the path that present
// actually takes, and the plane id has to be cleared so nothing downstream
// reads a stale "still on a plane".
//
// None of that is reachable from a mock. It needs a DrmCompositor, which needs
// a DrmBackend, which needs a card, GBM and EGL. vkms supplies the card: it is
// display-only, needs no root beyond membership of the video group, and is
// already what the leased-lease tier uses. Every case skips rather than fails
// when it is absent.
//
//   sudo modprobe vkms
//   ./homescreen_drm_backend_vkms_ut_test_driver

#include "backend/backend_registry.h"
#include "backend/drm_kms_egl/drm_backend.h"
#include "backend/drm_kms_egl/drm_compositor.h"
#include "backend/register_backends.h"
#include "configuration/configuration.h"
#include "display/drm_display.h"
#include "logging/logger.hpp"
#include "platform/homescreen/flutter_desktop_engine_state.h"
#include "platform/homescreen/flutter_desktop_view_controller_state.h"
#include "platform/homescreen/platform_views/egl_dmabuf_import.h"
#include "platform/homescreen/platform_views/platform_view_host.h"
#include "platform/homescreen/platform_views/platform_view_registry.h"
// FlutterDesktopViewControllerState only forward-declares TextInputPlugin but
// holds it by unique_ptr, so destroying a controller state needs the complete
// type here.
#include "platform/homescreen/text_input_plugin.h"
#include "view/compositor_surface_interface.h"
#include "view/flutter_view.h"

#include "ihs/platform_view.h"
#include "ihs/platform_view_host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

extern "C" {
#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#include <GLES2/gl2.h>

#if IHS_TEST_HAVE_CAPTURE
#include "capture/png.hpp"
#include "capture/snapshot.hpp"
#endif

#include <gtest/gtest.h>

#include "task_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

// The vkms card and the connector's preferred mode, or empty when there is no
// vkms to be had.
struct VkmsCard {
  std::string path;
  uint32_t mode_w{0};
  uint32_t mode_h{0};
  // What the connector scan saw, so a skip can say which half failed.
  int connectors{0};
  int connected{0};
  // Overlay planes the card exposes. Zero on a default vkms: enable_overlay
  // defaults off, and without an overlay the driver probe disables the plane
  // compositor, so the framed path does not exist to be tested.
  int overlays{0};

  [[nodiscard]] bool ok() const { return !path.empty() && mode_w != 0; }
};

// Descriptors this process holds. What it measures across a loop is what the
// loop left behind, so callers compare two samples rather than a ceiling.
// Same approach as mcp_transport-test and lease_client-test.
int CountOpenFds() {
  int count = 0;
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) {
    return 0;
  }
  while (::readdir(dir) != nullptr) {
    ++count;
  }
  ::closedir(dir);
  return count;
}

// Overlay planes on @p fd. Needs UNIVERSAL_PLANES, which is also what the
// backend sets before it counts them.
int CountOverlayPlanes(int fd) {
  if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
    return 0;
  }
  drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
  if (pres == nullptr) {
    return 0;
  }
  int overlays = 0;
  for (uint32_t i = 0; i < pres->count_planes; ++i) {
    drmModeObjectProperties* props =
        drmModeObjectGetProperties(fd, pres->planes[i], DRM_MODE_OBJECT_PLANE);
    if (props == nullptr) {
      continue;
    }
    for (uint32_t p = 0; p < props->count_props; ++p) {
      drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[p]);
      if (prop == nullptr) {
        continue;
      }
      if (std::string(prop->name) == "type" &&
          props->prop_values[p] == DRM_PLANE_TYPE_OVERLAY) {
        overlays++;
      }
      drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
  }
  drmModeFreePlaneResources(pres);
  return overlays;
}

VkmsCard FindVkms() {
  VkmsCard out;
  for (int i = 0; i < 8; ++i) {
    const std::string path = "/dev/dri/card" + std::to_string(i);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        v != nullptr && v->name != nullptr && std::string(v->name) == "vkms";
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    if (!is_vkms) {
      ::close(fd);
      continue;
    }
    out.path = path;
    out.overlays = CountOverlayPlanes(fd);
    if (drmModeRes* res = drmModeGetResources(fd); res != nullptr) {
      for (int c = 0; c < res->count_connectors; ++c) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
        if (conn == nullptr) {
          continue;
        }
        out.connectors++;
        if (conn->connection == DRM_MODE_CONNECTED) {
          out.connected++;
        }
        // First usable connector wins; the loop still visits the rest so the
        // counts above describe the whole card rather than the prefix that was
        // scanned before a match.
        if (out.mode_w == 0 && conn->connection == DRM_MODE_CONNECTED &&
            conn->count_modes > 0) {
          out.mode_w = conn->modes[0].hdisplay;
          out.mode_h = conn->modes[0].vdisplay;
        }
        drmModeFreeConnector(conn);
      }
      drmModeFreeResources(res);
    }
    ::close(fd);
    break;
  }
  return out;
}

// A platform-view surface that reports a GL texture and records what the
// compositor did to it.
//
// The buffer id it reports is 0 on purpose. 0 is ring slot 0, not a "this
// surface does not track ids" sentinel, and treating it as one is what left
// slot 0 unreleased: every release-fence timeout in a 100k-splat run was slot
// 0 and no other. A fake that reported 1 would pass against the broken code.
class FakePlatformView : public ICompositorSurface {
 public:
  explicit FakePlatformView(FlutterPlatformViewIdentifier id) : id_(id) {}

  bool OnCreateBackingStore(const FlutterBackingStoreConfig*,
                            FlutterBackingStore*) override {
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override {
    return true;
  }
  bool OnPresent(const FlutterLayer*) override {
    presents_++;
    return true;
  }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return id_;
  }

  // Counted: for a real view each call may import the latest frame.
  [[nodiscard]] uint32_t GetGlTextureName() const override {
    imports_++;
    return tex_;
  }
  [[nodiscard]] int32_t GetGlTextureWidth() const override { return 16; }
  [[nodiscard]] int32_t GetGlTextureHeight() const override { return 16; }
  [[nodiscard]] uint32_t GetGlTextureBufferId() const override { return 0; }
  [[nodiscard]] GlLayerTexture GetLayerGlTexture(size_t index) const override {
    GlLayerTexture out = ICompositorSurface::GetLayerGlTexture(index);
    if (cropped_) {
      out.geometry.src = RectF{0, 0, 8, 8};
    }
    return out;
  }

  void OnScanoutRelease(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    released_.push_back(buffer_id);
  }
  void SetScanoutPlane(uint32_t plane_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    planes_.push_back(plane_id);
  }

  void set_texture(uint32_t tex) { tex_ = tex; }
  // Show a crop of the image, which takes the layered draw path.
  void set_cropped(bool cropped) { cropped_ = cropped; }
  [[nodiscard]] int32_t width() const { return 16; }
  [[nodiscard]] int presents() const { return presents_; }
  [[nodiscard]] int imports() const { return imports_; }

  [[nodiscard]] std::vector<uint32_t> released() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return released_;
  }
  [[nodiscard]] std::vector<uint32_t> planes() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return planes_;
  }

 private:
  const FlutterPlatformViewIdentifier id_;
  uint32_t tex_{0};
  bool cropped_{false};
  int presents_{0};
  mutable int imports_{0};
  mutable std::mutex mu_;
  std::vector<uint32_t> released_;
  std::vector<uint32_t> planes_;
};

// A scanout-capable dma-buf filled with a flat color.
//
// Allocated through GBM on the card itself. vkms has no render node, so there
// is no GPU to render into it -- but the scene path never renders a platform
// view's buffer, it hands it straight to a KMS plane, which is the whole point
// of that path and exactly what this has to exercise. Linear and XRGB8888
// because that is what the plane accepts and what makes the CPU fill trivial.
class GbmSolidBuffer {
 public:
  bool Create(int card_fd, uint32_t w, uint32_t h, uint32_t argb) {
    dev_ = gbm_create_device(card_fd);
    if (dev_ == nullptr) {
      return false;
    }
    bo_ = gbm_bo_create(dev_, w, h, GBM_FORMAT_XRGB8888,
                        GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (bo_ == nullptr) {
      return false;
    }
    void* map_data = nullptr;
    uint32_t stride = 0;
    void* ptr =
        gbm_bo_map(bo_, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (ptr == nullptr) {
      return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
      auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ptr) +
                                              static_cast<size_t>(y) * stride);
      for (uint32_t x = 0; x < w; ++x) {
        row[x] = argb;
      }
    }
    gbm_bo_unmap(bo_, map_data);
    w_ = w;
    h_ = h;
    stride_ = gbm_bo_get_stride(bo_);
    return true;
  }

  ~GbmSolidBuffer() {
    if (bo_ != nullptr) {
      gbm_bo_destroy(bo_);
    }
    if (dev_ != nullptr) {
      gbm_device_destroy(dev_);
    }
  }

  GbmSolidBuffer() = default;
  GbmSolidBuffer(const GbmSolidBuffer&) = delete;
  GbmSolidBuffer& operator=(const GbmSolidBuffer&) = delete;

  // A fresh owned handle each call, which is the ownership GetDmabuf promises.
  [[nodiscard]] int ExportFd() const {
    return bo_ != nullptr ? gbm_bo_get_fd(bo_) : -1;
  }
  [[nodiscard]] uint32_t width() const { return w_; }
  [[nodiscard]] uint32_t height() const { return h_; }
  [[nodiscard]] uint32_t stride() const { return stride_; }

 private:
  gbm_device* dev_{nullptr};
  gbm_bo* bo_{nullptr};
  uint32_t w_{0};
  uint32_t h_{0};
  uint32_t stride_{0};
};

// A platform view whose content is a dma-buf, which is what the scene path
// drives. The texture-only fake above is invisible to it: GetDmabuf defaults to
// kNotScanoutCapable, so the scene path never presents such a surface at all.
class FakeDmabufPlatformView : public ICompositorSurface {
 public:
  FakeDmabufPlatformView(FlutterPlatformViewIdentifier id,
                         const GbmSolidBuffer& buffer,
                         uint32_t buffer_id)
      : id_(id), buffer_(&buffer), buffer_id_(buffer_id) {}

  bool OnCreateBackingStore(const FlutterBackingStoreConfig*,
                            FlutterBackingStore*) override {
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override {
    return true;
  }
  bool OnPresent(const FlutterLayer*) override {
    presents_++;
    if (gl_held_) {
      held_presents_++;
    }
    return true;
  }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return id_;
  }
  void HoldGlFrames(bool hold) override { gl_held_ = hold; }

  // Deliver-once, like the real producer: a frame reaches the scanout path at
  // most once, and a present with nothing new says so rather than handing the
  // same buffer over twice.
  [[nodiscard]] DmabufState GetDmabuf(Dmabuf* out) const override {
    const std::lock_guard<std::mutex> lock(mu_);
    ++polls_;
    if (!fresh_) {
      return DmabufState::kNoNewFrame;
    }
    const int fd = buffer_->ExportFd();
    if (fd < 0) {
      return DmabufState::kNotScanoutCapable;
    }
    // Offered, not consumed: like the real view host, the frame stays
    // eligible until the compositor acks it onto a source or hands the slot
    // back, so an import that fails downstream is offered again (#332).
    had_content_ = true;
    out->fd[0] = fd;
    out->fourcc = DRM_FORMAT_XRGB8888;
    out->modifier = DRM_FORMAT_MOD_LINEAR;
    // Zero dimensions are what ExternalDmaBufPool::create rejects outright,
    // so this is a frame the compositor cannot import however it tries.
    out->width = bad_frame_ ? 0 : buffer_->width();
    out->height = bad_frame_ ? 0 : buffer_->height();
    out->plane_count = 1;
    out->offset[0] = 0;
    out->stride[0] = buffer_->stride();
    out->acquire_fence_fd = -1;  // filled by the CPU, already synced
    out->buffer_id = buffer_id_;
    delivered_++;
    return DmabufState::kFrame;
  }

  ~FakeDmabufPlatformView() override {
    if (release_fence_fd_ >= 0) {
      ::close(release_fence_fd_);
    }
  }

  void OnScanoutRelease(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    if (buffer_id == buffer_id_) {
      fresh_ = false;  // terminal: the slot is the producer's again
    }
    released_.push_back(buffer_id);
  }
  void AckDmabufScanout(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    if (buffer_id == buffer_id_) {
      fresh_ = false;
    }
    acked_.push_back(buffer_id);
  }
  [[nodiscard]] bool HasContent() const override {
    const std::lock_guard<std::mutex> lock(mu_);
    return had_content_;
  }
  void SetScanoutPlane(uint32_t plane_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    planes_.push_back(plane_id);
  }

  // The compositor's release fence for this view (#513). Mirrors the real view
  // host: take ownership, superseding any previous one.
  void SetReleaseFenceFd(int fd) override {
    const std::lock_guard<std::mutex> lock(mu_);
    if (release_fence_fd_ >= 0) {
      ::close(release_fence_fd_);
    }
    release_fence_fd_ = fd;
    if (fd >= 0) {
      ++release_fences_;
    }
  }

  [[nodiscard]] int release_fences() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return release_fences_;
  }

  // Whether the fence handed back actually signals. A real OUT_FENCE does once
  // its commit has been scanned out; an fd that is not a live sync_file would
  // not, which is what separates "a fence arrived" from "a number arrived".
  [[nodiscard]] bool ReleaseFenceSignals(int timeout_ms) const {
    const std::lock_guard<std::mutex> lock(mu_);
    if (release_fence_fd_ < 0) {
      return false;
    }
    pollfd pfd{release_fence_fd_, POLLIN, 0};
    return ::poll(&pfd, 1, timeout_ms) == 1 && (pfd.revents & POLLIN) != 0;
  }

  // Stand in for the producer submitting again, optionally from a different
  // ring slot -- which is what makes the previous slot releasable.
  void Submit(const GbmSolidBuffer* buffer = nullptr, uint32_t buffer_id = 0) {
    const std::lock_guard<std::mutex> lock(mu_);
    if (buffer != nullptr) {
      buffer_ = buffer;
      buffer_id_ = buffer_id;
    }
    fresh_ = true;
  }

  // How many layers the view says it draws. The plane path wants a plane for
  // each; only layer 0 has a frame.
  [[nodiscard]] size_t GetLayerCount() const override {
    const std::lock_guard<std::mutex> lock(mu_);
    return layer_count_;
  }
  void set_layer_count(size_t n) {
    const std::lock_guard<std::mutex> lock(mu_);
    layer_count_ = n;
  }

  [[nodiscard]] int presents() const { return presents_; }
  // Presents composited while held to the frame already bound, and whether
  // the hold is still on.
  [[nodiscard]] int held_presents() const { return held_presents_; }
  [[nodiscard]] bool gl_held() const { return gl_held_; }
  // Times the plane path asked for a frame, fresh or not.
  [[nodiscard]] int polls() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return polls_;
  }
  [[nodiscard]] int delivered() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return delivered_;
  }
  [[nodiscard]] std::vector<uint32_t> released() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return released_;
  }
  [[nodiscard]] std::vector<uint32_t> planes() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return planes_;
  }
  [[nodiscard]] std::vector<uint32_t> acked() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return acked_;
  }
  // Offer a frame the compositor cannot import, so the failure lands after
  // GetDmabuf has already handed the frame over -- the case #332 is about.
  void set_offers_unimportable_frame(bool v) {
    const std::lock_guard<std::mutex> lock(mu_);
    bad_frame_ = v;
  }

 private:
  const FlutterPlatformViewIdentifier id_;
  const GbmSolidBuffer* buffer_;
  uint32_t buffer_id_;
  int presents_{0};
  int held_presents_{0};
  bool gl_held_{false};
  mutable std::mutex mu_;
  mutable bool fresh_{true};
  mutable bool had_content_{false};
  bool bad_frame_{false};
  mutable int delivered_{0};
  mutable int polls_{0};
  size_t layer_count_{1};
  std::vector<uint32_t> acked_;
  std::vector<uint32_t> released_;
  std::vector<uint32_t> planes_;
  int release_fence_fd_{-1};
  int release_fences_{0};
};

// A backend on vkms with a framed output, which is the configuration #530 was
// found in: a framebuffer smaller than the mode puts every present through
// PresentFramed, the one path that queued platform-view releases and never
// drained them.
class DrmBackendVkmsBase : public ::testing::Test {
 protected:
  // Which present path this fixture pins. Never left to DriverProbe: see the
  // comment on cfg.compositor below.
  virtual void Configure(DrmConfig& cfg) = 0;

  // The framebuffer the compositor draws into, which a full-size layer has to
  // match. Set by Configure; the backend keeps its own copy privately.
  uint32_t content_w_{0};
  uint32_t content_h_{0};

  void SetUp() override {
    card_ = FindVkms();
    // Two different failures, reported as two different messages. Conflating
    // them cost a CI round trip: the runner had a vkms card the whole time and
    // the skip said "no connected vkms card", which reads as "no card".
    if (card_.path.empty()) {
      GTEST_SKIP() << "no vkms card on this host (sudo modprobe vkms)";
    }
    if (!card_.ok()) {
      GTEST_SKIP() << card_.path << " is vkms but exposes no connected "
                   << "connector with modes (connectors=" << card_.connectors
                   << " connected=" << card_.connected << ")";
    }

    display_ = std::make_unique<DrmDisplay>(0, 0, 0.0, card_.path,
                                            /*no_seat=*/true);
    drm::Device* dev = display_->SharedDevice();
    if (dev == nullptr) {
      GTEST_SKIP() << "no DRM master on " << card_.path;
    }

    DrmConfig cfg{card_.path, std::nullopt, std::nullopt,
                  /*debug_backend=*/false};
    cfg.no_seat = true;
    cfg.disable_cursor = true;
    // Pin the GL compositor rather than letting DriverProbe choose.
    //
    // These cases are about the GL-composited platform-view path, and with
    // kAuto the path is decided by whether the card happens to expose overlay
    // planes -- which for vkms is a module parameter (enable_overlay). This
    // test was written against a vkms with no overlays, where the probe fell
    // back to GL on its own; loading the module with overlays turned it onto
    // the scene path, where a platform view is driven through GetDmabuf and a
    // texture-only surface is never presented at all. Three cases went from
    // passing to failing with no code change on either side.
    //
    // The scene path deserves its own fixture and its own fake. What it must
    // not be is whichever one the host's module parameters select today.
    Configure(cfg);
    backend_ =
        DrmBackend::Create(cfg, display_->session(), dev, display_.get());
    ASSERT_NE(backend_, nullptr) << "DrmBackend::Create failed on vkms";

    compositor_ = backend_->compositor();
    ASSERT_NE(compositor_, nullptr) << "built without BUILD_COMPOSITOR";

    // Start the card's flip reader, exactly as register_backends.cc does right
    // after Create.
    //
    // Not optional bookkeeping: WaitForPendingFlip waits on a flag that only
    // this thread clears, so without it every present after the first burns the
    // full 100 ms timeout and logs "PAGE_FLIP_EVENT likely lost". The GL and
    // framed cases still passed that way -- the wait returns true on timeout
    // and the drain runs regardless -- but the scene path's scanout release is
    // driven by the flip completion itself, so it never fired at all.
    display_->SetFlipHandler(&DrmBackend::UnifiedPageFlipHandler);
    display_->StartFlipReader();

    // A texture the compositor can actually sample. It never has to contain
    // anything: the assertions are about the release bookkeeping around the
    // composite, not about pixels.
    ASSERT_TRUE(backend_->MakeCurrent());
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    ASSERT_NE(tex_, 0u);
  }

  void TearDown() override {
    if (backend_ && tex_ != 0) {
      backend_->MakeCurrent();
      glDeleteTextures(1, &tex_);
    }
    backend_.reset();
    display_.reset();
  }

  // Paint the fixture's texture a flat color, so a snapshot can say whether
  // the compositor actually sampled it. Bytes are R,G,B,A in GL memory order.
  void PaintTexture(uint8_t r, uint8_t g, uint8_t b) {
    std::array<uint8_t, 16 * 16 * 4> px{};
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i + 0] = r;
      px[i + 1] = g;
      px[i + 2] = b;
      px[i + 3] = 0xFF;
    }
    ASSERT_TRUE(backend_->MakeCurrent());
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px.data());
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
  }

#if IHS_TEST_HAVE_CAPTURE
  // Read the CRTC's plane composition back. drm-cxx maps each scanout FB and
  // composites in zpos order, so this is what the display is actually showing
  // -- not what the compositor believes it drew.
  //
  // Writes a PNG alongside when IHS_TEST_CAPTURE_DIR names a directory, which
  // is how a frame from this test gets looked at rather than only asserted on.
  drm::capture::Image Snapshot(const char* name) {
    auto shot = drm::capture::snapshot(backend_->device(), backend_->crtc_id());
    if (!shot) {
      ADD_FAILURE() << "snapshot failed: " << shot.error().message();
      return {};
    }
    drm::capture::Image image = std::move(shot.value());
    if (const char* dir = std::getenv("IHS_TEST_CAPTURE_DIR"); dir != nullptr) {
      const std::string path = std::string(dir) + "/" + name + ".png";
      if (auto w = drm::capture::write_png(image, path); !w) {
        ADD_FAILURE() << "write_png: " << w.error().message();
      } else {
        std::cerr << "capture: " << path << "\n";
      }
    }
    return image;
  }

  // The pixel at the center of the CRTC, as 0xAARRGGBB.
  static uint32_t CenterPixel(const drm::capture::Image& img) {
    if (img.empty()) {
      return 0;
    }
    const size_t i =
        static_cast<size_t>(img.height() / 2) * img.width() + img.width() / 2;
    return img.pixels()[i];
  }

  // Assert the CRTC's center pixel is @p rgb, and save the frame when asked.
  void ExpectCenterColor(const char* name, uint32_t rgb) {
    const drm::capture::Image image = Snapshot(name);
    ASSERT_FALSE(image.empty());
    EXPECT_EQ(image.width(), card_.mode_w);
    EXPECT_EQ(image.height(), card_.mode_h);
    // RGB only: the primary is XRGB8888, so the alpha byte carries no meaning.
    EXPECT_EQ(CenterPixel(image) & 0x00FFFFFFu, rgb)
        << "the center of the display is not the color that was expected";
  }
#endif  // IHS_TEST_HAVE_CAPTURE

  // Present until @p done holds, or give up after @p max frames.
  //
  // The scanout release is asynchronous by construction: the pool fires it when
  // the flip that displaced the buffer completes, on the card's flip-reader
  // thread, and the compositor drains the queue at the top of a later present.
  // How many presents that takes is a scheduling detail -- measured here as two
  // or three -- so a test that pins the count is pinning the weather. What is
  // contractual is that it arrives.
  bool PumpUntil(FlutterPlatformViewIdentifier id,
                 const std::function<bool()>& done,
                 int max = 10) {
    for (int i = 0; i < max; ++i) {
      if (!PresentPlatformView(id)) {
        return false;
      }
      if (done()) {
        return true;
      }
    }
    return false;
  }

  // One frame carrying nothing but the platform view.
  bool PresentPlatformView(FlutterPlatformViewIdentifier id) {
    FlutterPlatformView pv{};
    pv.struct_size = sizeof(FlutterPlatformView);
    pv.identifier = id;

    FlutterLayer layer{};
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypePlatformView;
    layer.platform_view = &pv;
    layer.offset = FlutterPoint{0.0, 0.0};
    layer.size = FlutterSize{static_cast<double>(layer_w_),
                             static_cast<double>(layer_h_)};

    const FlutterLayer* layers[] = {&layer};
    return compositor_->PresentLayers(layers, 1);
  }

  VkmsCard card_;
  std::unique_ptr<DrmDisplay> display_;
  std::unique_ptr<DrmBackend> backend_;
  DrmCompositor* compositor_{nullptr};
  uint32_t tex_{0};
  // Layer extent for PresentPlatformView. Small by default; the pixel cases
  // set it to the whole framebuffer so the center of the CRTC lands inside it
  // whatever the letterboxing.
  uint32_t layer_w_{16};
  uint32_t layer_h_{16};
};

// The plane compositor, full screen: PresentLayersViaScene -- the zero-copy
// path, where a platform view's own buffer goes on a KMS plane and the
// compositor never touches its pixels. Needs overlay planes, same as the framed
// fixture.
class DrmBackendVkmsScene : public DrmBackendVkmsBase {
 protected:
  void Configure(DrmConfig& cfg) override {
    if (card_.overlays == 0) {
      GTEST_SKIP() << "vkms has no overlay plane, so there is no scene path "
                      "to test (modprobe vkms enable_overlay=1)";
    }
    cfg.compositor = drm_config::Compositor::kPlanes;
    content_w_ = card_.mode_w;
    content_h_ = card_.mode_h;
  }
};

// The GL compositor, full screen: PresentViaGlFallback.
class DrmBackendVkms : public DrmBackendVkmsBase {
 protected:
  void Configure(DrmConfig& cfg) override {
    cfg.compositor = drm_config::Compositor::kGl;
    content_w_ = card_.mode_w;
    content_h_ = card_.mode_h;
  }
};

// The plane compositor with a framebuffer smaller than the mode, which is what
// puts every present through PresentFramed -- the path #530 was found on, and
// the one the GL fixture above cannot reach. Needs a card with overlay planes;
// on vkms that is the enable_overlay module parameter, so the fixture checks
// for one rather than assuming.
class DrmBackendVkmsFramed : public DrmBackendVkmsBase {
 protected:
  void Configure(DrmConfig& cfg) override {
    if (card_.overlays == 0) {
      GTEST_SKIP() << "vkms has no overlay plane, so there is no framed path "
                      "to test (modprobe vkms enable_overlay=1)";
    }
    cfg.compositor = drm_config::Compositor::kPlanes;
    cfg.width = card_.mode_w - 64;
    cfg.height = card_.mode_h - 64;
    content_w_ = *cfg.width;
    content_h_ = *cfg.height;
  }
};

// The whole of #530 in one case: present a GL-composited view twice and its
// producer gets buffer 0 back.
//
// Twice because the release is deferred by design -- the compositor re-samples
// a bound texture until a newer one arrives, so handing it back during the
// present that sampled it would return a slot still being drawn from. The
// second present's flip wait is what makes it safe, and draining there is
// exactly what PresentFramed did not do.
TEST_F(DrmBackendVkms, GlCompositedViewGetsItsBufferBack) {
  auto view = std::make_shared<FakePlatformView>(7);
  view->set_texture(tex_);
  compositor_->RegisterSurface(7, view);

  ASSERT_TRUE(PresentPlatformView(7));
  EXPECT_EQ(view->presents(), 1);
  EXPECT_TRUE(view->released().empty())
      << "released during the present that sampled it";

  ASSERT_TRUE(PresentPlatformView(7));
  const std::vector<uint32_t> released = view->released();
  ASSERT_FALSE(released.empty())
      << "the deferred release was queued and never drained (#530)";
  EXPECT_EQ(released.front(), 0u) << "buffer id 0 is ring slot 0, not a "
                                     "sentinel for 'no id'";

  compositor_->UnregisterSurface(7);
}

// A present takes a view's texture once and draws what it took. It used to
// take it to decide how to draw the view and again to draw it, and a frame
// submitted in between was imported over one nothing had drawn: no release was
// ever queued for that one, and its producer waited for good.
TEST_F(DrmBackendVkms, APresentTakesAViewsTextureOnce) {
  for (const bool cropped : {false, true}) {
    SCOPED_TRACE(cropped ? "cropped" : "whole");
    auto view = std::make_shared<FakePlatformView>(21);
    view->set_texture(tex_);
    view->set_cropped(cropped);
    compositor_->RegisterSurface(21, view);
    ASSERT_TRUE(PresentPlatformView(21));
    EXPECT_EQ(view->imports(), 1);
    compositor_->UnregisterSurface(21);
  }
}

TEST_F(DrmBackendVkmsFramed, APresentTakesAViewsTextureOnce) {
  for (const bool cropped : {false, true}) {
    SCOPED_TRACE(cropped ? "cropped" : "whole");
    auto view = std::make_shared<FakePlatformView>(22);
    view->set_texture(tex_);
    view->set_cropped(cropped);
    compositor_->RegisterSurface(22, view);
    ASSERT_TRUE(PresentPlatformView(22));
    EXPECT_EQ(view->imports(), 1);
    compositor_->UnregisterSurface(22);
  }
}

// A GL-composited view is on no plane, and has to be told so. Anything keyed
// on "is this view on a plane" -- the supersede guard in the view host, the
// DRM_PLANE grant accessor -- otherwise keeps whatever id the scene path last
// set and reads a stale yes.
TEST_F(DrmBackendVkms, GlCompositedViewIsReportedOffAnyPlane) {
  auto view = std::make_shared<FakePlatformView>(9);
  view->set_texture(tex_);
  compositor_->RegisterSurface(9, view);

  ASSERT_TRUE(PresentPlatformView(9));

  const std::vector<uint32_t> planes = view->planes();
  ASSERT_FALSE(planes.empty()) << "the present never reported a plane at all";
  EXPECT_EQ(planes.back(), 0u) << "GL-composited, so no plane";

  compositor_->UnregisterSurface(9);
}

// A surface that exposes no texture is not composited, so nothing is queued
// against it and nothing is released. Guards the other direction of the
// buffer-id-0 fix: dropping the zero check must not start releasing frames
// for views the compositor never sampled.
TEST_F(DrmBackendVkms, AViewWithNoTextureIsNeverReleased) {
  auto view = std::make_shared<FakePlatformView>(11);  // texture stays 0
  compositor_->RegisterSurface(11, view);

  ASSERT_TRUE(PresentPlatformView(11));
  ASSERT_TRUE(PresentPlatformView(11));

  EXPECT_EQ(view->presents(), 2);
  EXPECT_TRUE(view->released().empty());

  compositor_->UnregisterSurface(11);
}

// The same release contract on PresentFramed. This is the path #530 was found
// on -- a framed config on a Pi 5 -- and it queued platform-view releases
// without ever draining them. The GL fixture above cannot reach it: framing
// requires the atomic plane compositor, so the framed path only exists on a
// card with overlay planes.
TEST_F(DrmBackendVkmsFramed, GlCompositedViewGetsItsBufferBack) {
  auto view = std::make_shared<FakePlatformView>(13);
  view->set_texture(tex_);
  compositor_->RegisterSurface(13, view);

  ASSERT_TRUE(PresentPlatformView(13));
  EXPECT_EQ(view->presents(), 1);
  EXPECT_TRUE(view->released().empty())
      << "released during the present that sampled it";

  ASSERT_TRUE(PresentPlatformView(13));
  const std::vector<uint32_t> released = view->released();
  ASSERT_FALSE(released.empty())
      << "the deferred release was queued and never drained (#530)";
  EXPECT_EQ(released.front(), 0u);

  compositor_->UnregisterSurface(13);
}

TEST_F(DrmBackendVkmsFramed, GlCompositedViewIsReportedOffAnyPlane) {
  auto view = std::make_shared<FakePlatformView>(15);
  view->set_texture(tex_);
  compositor_->RegisterSurface(15, view);

  ASSERT_TRUE(PresentPlatformView(15));

  const std::vector<uint32_t> planes = view->planes();
  ASSERT_FALSE(planes.empty()) << "the present never reported a plane at all";
  EXPECT_EQ(planes.back(), 0u) << "GL-composited into the framed buffer, so "
                                  "on no plane of its own";

  compositor_->UnregisterSurface(15);
}

#if IHS_TEST_HAVE_CAPTURE
// The compositor drew the plugin's texture, not just bookkeeping around it.
//
// Every other case here asserts on which callbacks fired, which says nothing
// about whether a pixel moved. This reads the CRTC's plane composition back --
// drm-cxx maps each scanout FB and composites in zpos order -- and checks the
// color the fake uploaded is the color on screen at the center of the
// display. A view that is registered, presented and released correctly but
// composited from the wrong texture, at the wrong scale, or into a buffer
// nothing scans out would pass everything above and fail here.
TEST_F(DrmBackendVkms, TheViewsTextureReachesTheDisplay) {
  auto view = std::make_shared<FakePlatformView>(17);
  view->set_texture(tex_);
  PaintTexture(0x20, 0x80, 0xC0);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(17, view);

  ASSERT_TRUE(PresentPlatformView(17));

  ExpectCenterColor("gl_platform_view", 0x002080C0u);

  compositor_->UnregisterSurface(17);
}

TEST_F(DrmBackendVkmsFramed, TheViewsTextureReachesTheDisplay) {
  auto view = std::make_shared<FakePlatformView>(19);
  view->set_texture(tex_);
  PaintTexture(0xC0, 0x40, 0x20);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(19, view);

  ASSERT_TRUE(PresentPlatformView(19));

  // The CRTC is the mode; the content is the smaller FB centered in it. The
  // center pixel is inside the content either way, which is the point of
  // sampling there rather than at a corner.
  ExpectCenterColor("framed_platform_view", 0x00C04020u);

  compositor_->UnregisterSurface(19);
}
#endif  // IHS_TEST_HAVE_CAPTURE

// The zero-copy path end to end: a producer's own dma-buf lands on a KMS plane
// and its pixels reach the display without the compositor drawing anything.
//
// This is the path the other fixtures cannot reach. A texture-only surface is
// invisible to it -- GetDmabuf defaults to kNotScanoutCapable, so the scene
// path routes the whole frame to GL and the view is never placed at all, which
// is exactly what happened when overlay planes first appeared on this host.
TEST_F(DrmBackendVkmsScene, AProducersBufferScansOutOnItsOwnPlane) {
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF1E7A46u))
      << "could not allocate a scanout dma-buf on the card";

  auto view = std::make_shared<FakeDmabufPlatformView>(21, buffer, 0);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(21, view);

  ASSERT_TRUE(PresentPlatformView(21));
  EXPECT_EQ(view->delivered(), 1) << "the scene path never pulled the frame";

  const std::vector<uint32_t> planes = view->planes();
  ASSERT_FALSE(planes.empty())
      << "the view was never told which plane it is on";
  EXPECT_NE(planes.back(), 0u)
      << "reported off any plane, so it was GL-composited rather than scanned "
         "out -- the zero-copy path did not run";

#if IHS_TEST_HAVE_CAPTURE
  // The producer's buffer is what the display is showing. Only available with
  // blend2d; the plane assertions above run either way.
  ExpectCenterColor("scene_platform_view", 0x001E7A46u);
#endif

  compositor_->UnregisterSurface(21);
}

// The scanout release, which is the mechanism the GL paths lack and had to
// emulate. A second buffer displaces the first; once the flip that sampled it
// has completed, the first slot comes back to the producer.
TEST_F(DrmBackendVkmsScene, TheDisplacedSlotComesBackToTheProducer) {
  GbmSolidBuffer first;
  GbmSolidBuffer second;
  ASSERT_TRUE(first.Create(backend_->device().fd(), content_w_, content_h_,
                           0xFF1E7A46u));
  ASSERT_TRUE(second.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF7A1E46u));

  // Slot 0 first, deliberately: 0 is a ring slot like any other, and it is the
  // value that was mistaken for "no id" on the GL side.
  auto view = std::make_shared<FakeDmabufPlatformView>(23, first, 0);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(23, view);

  ASSERT_TRUE(PresentPlatformView(23));
  EXPECT_TRUE(view->released().empty())
      << "released while it was the frame on the plane";

  view->Submit(&second, 1);
  ASSERT_TRUE(PumpUntil(23, [&] { return !view->released().empty(); }))
      << "slot 0 never came back";

  const std::vector<uint32_t> released = view->released();
  EXPECT_EQ(released.front(), 0u)
      << "buffer id 0 is ring slot 0, not a sentinel for 'no id'";
  EXPECT_EQ(view->delivered(), 2)
      << "GetDmabuf is deliver-once; a present with no new frame must not "
         "pull the same buffer again";

#if IHS_TEST_HAVE_CAPTURE
  // And the newer buffer is what is on screen.
  ExpectCenterColor("scene_second_buffer", 0x007A1E46u);
#endif

  compositor_->UnregisterSurface(23);
}

// More layers than the CRTC has planes: the frame is composited without the
// plane path taking a frame from the producer or importing anything, and the
// view is told it is on no plane.
TEST_F(DrmBackendVkmsScene, MoreLayersThanPlanesSkipsThePlanePath) {
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF1E7A46u));
  auto view = std::make_shared<FakeDmabufPlatformView>(25, buffer, 0);
  view->set_layer_count(64);  // more than any vkms has planes
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(25, view);

  ASSERT_TRUE(PresentPlatformView(25));
  ASSERT_TRUE(PresentPlatformView(25));

  EXPECT_EQ(view->polls(), 0)
      << "the plane path asked for a frame it could never place";
  const std::vector<uint32_t> planes = view->planes();
  ASSERT_FALSE(planes.empty());
  EXPECT_EQ(planes.back(), 0U) << "composited, so on no plane";

  // Back within the budget, the plane path is used again.
  view->set_layer_count(1);
  ASSERT_TRUE(PresentPlatformView(25));
  EXPECT_GT(view->polls(), 0);
  EXPECT_NE(view->planes().back(), 0U);

  compositor_->UnregisterSurface(25);
}

// A frame on a plane when GL takes the CRTC back is released, though the scene
// that holds it does not commit again. Its view stays, but every frame after
// needs more planes than the CRTC has.
TEST_F(DrmBackendVkmsScene, AFrameOnAPlaneIsReleasedWhenGlTakesTheCrtc) {
  GbmSolidBuffer first;
  GbmSolidBuffer second;
  ASSERT_TRUE(first.Create(backend_->device().fd(), content_w_, content_h_,
                           0xFF1E7A46u));
  ASSERT_TRUE(second.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF7A1E46u));
  auto view = std::make_shared<FakeDmabufPlatformView>(28, first, 0);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(28, view);

  ASSERT_TRUE(PresentPlatformView(28));
  ASSERT_NE(view->planes().back(), 0U) << "the first frame is on no plane";
  ASSERT_TRUE(view->released().empty());

  view->set_layer_count(64);  // more than any vkms has planes
  view->Submit(&second, 1);
  EXPECT_TRUE(PumpUntil(28, [&] {
    const auto r = view->released();
    return std::find(r.begin(), r.end(), 0U) != r.end();
  })) << "the frame the scene held was never released";

  // Its next frame that fits goes back on a plane.
  view->set_layer_count(1);
  view->Submit(&first, 0);
  EXPECT_TRUE(PumpUntil(28, [&] { return view->planes().back() != 0U; }))
      << "the plane path never took the view back";

  compositor_->UnregisterSurface(28);
}

// The same when the view leaves: no later frame fits the planes, so the scene
// never commits again to retire the view's layer.
TEST_F(DrmBackendVkmsScene, AFrameOnAPlaneIsReleasedWhenItsViewLeavesForGl) {
  GbmSolidBuffer first;
  GbmSolidBuffer other;
  ASSERT_TRUE(first.Create(backend_->device().fd(), content_w_, content_h_,
                           0xFF1E7A46u));
  ASSERT_TRUE(other.Create(backend_->device().fd(), content_w_, content_h_,
                           0xFF7A1E46u));
  auto leaving = std::make_shared<FakeDmabufPlatformView>(29, first, 0);
  auto staying = std::make_shared<FakeDmabufPlatformView>(30, other, 0);
  staying->set_layer_count(64);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(29, leaving);
  compositor_->RegisterSurface(30, staying);

  ASSERT_TRUE(PresentPlatformView(29));
  ASSERT_NE(leaving->planes().back(), 0U) << "the frame is on no plane";
  ASSERT_TRUE(leaving->released().empty());

  EXPECT_TRUE(PumpUntil(30, [&] { return !leaving->released().empty(); }))
      << "the frame of the view that left was never released";

  compositor_->UnregisterSurface(30);
  compositor_->UnregisterSurface(29);
}

// A frame the allocator turns down is not asked about again on every present:
// the same shape goes straight to composition until the retry interval, so a
// producer's new frames stop being imported only to be refused.
TEST_F(DrmBackendVkmsScene, ARejectedFrameShapeIsNotRetriedEveryPresent) {
  // A buffer a quarter the size of its layer: placing it needs a scaling
  // plane, and vkms has none.
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), 32, 32, 0xFF1E7A46u));
  auto view = std::make_shared<FakeDmabufPlatformView>(27, buffer, 0);
  layer_w_ = 64;
  layer_h_ = 64;
  compositor_->RegisterSurface(27, view);

  ASSERT_TRUE(PresentPlatformView(27));
  const int first = view->polls();
  ASSERT_GT(first, 0) << "the plane path never looked at the view";
  ASSERT_EQ(view->planes().back(), 0U)
      << "vkms placed a scaled layer; this case needs a plane that cannot";

  for (int i = 0; i < 5; ++i) {
    view->Submit();
    ASSERT_TRUE(PresentPlatformView(27));
  }
  EXPECT_EQ(view->polls(), first)
      << "a frame shape the allocator just refused was offered to it again";

  // A frame of another shape is tried at once.
  layer_w_ = 32;
  layer_h_ = 32;
  view->Submit();
  ASSERT_TRUE(PresentPlatformView(27));
  EXPECT_GT(view->polls(), first);
  EXPECT_NE(view->planes().back(), 0U) << "an unscaled layer fits a plane";

  compositor_->UnregisterSurface(27);
}

// #332. GetDmabuf hands a frame over; that is not the same as the frame being
// taken. These three pin the difference down.

// The happy path: a frame that reaches a source is acked exactly once, and is
// not also released -- the two answers are exclusive.
TEST_F(DrmBackendVkmsScene, ATakenFrameIsAckedAndNotReleased) {
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF1E7A46u));

  auto view = std::make_shared<FakeDmabufPlatformView>(31, buffer, 7);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(31, view);

  ASSERT_TRUE(PresentPlatformView(31));

  EXPECT_EQ(view->acked(), std::vector<uint32_t>{7u})
      << "a frame the scene took was never acked, so the view host cannot "
         "tell it apart from one that failed to import";
  EXPECT_TRUE(view->released().empty())
      << "acked and released: the slot is accounted for twice";

  compositor_->UnregisterSurface(31);
}

// The failure path: an import that fails after the hand-off must not ack. The
// frame is instead given back, which is what tells the producer its slot is
// free again.
TEST_F(DrmBackendVkmsScene, AFrameThatFailedToImportIsNotAcked) {
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF1E7A46u));

  auto view = std::make_shared<FakeDmabufPlatformView>(32, buffer, 5);
  view->set_offers_unimportable_frame(true);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(32, view);

  ASSERT_TRUE(PresentPlatformView(32));

  EXPECT_TRUE(view->acked().empty())
      << "acked a frame that never reached a source; the producer would wait "
         "for a release that cannot come";
  EXPECT_FALSE(view->released().empty())
      << "neither acked nor released, so the ring slot is held forever";

  compositor_->UnregisterSurface(32);
}

// And the visible half. After that failed import the view has content but no
// new frame, and the present that follows must still composite it. Dropping it
// there is what blanked a static producer: it never submits again, so
// GetDmabuf never returns another frame and the view never comes back.
TEST_F(DrmBackendVkmsScene, AnIdleViewWithContentIsStillComposited) {
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF1E7A46u));

  auto view = std::make_shared<FakeDmabufPlatformView>(33, buffer, 3);
  view->set_offers_unimportable_frame(true);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(33, view);

  ASSERT_TRUE(PresentPlatformView(33));
  const int after_first = view->presents();
  ASSERT_GT(after_first, 0) << "the view was never presented at all";

  // No new frame from here on -- a static producer.
  ASSERT_TRUE(PresentPlatformView(33));

  EXPECT_GT(view->presents(), after_first)
      << "the view stopped being composited once its frame was consumed, so a "
         "static producer blanks";

  compositor_->UnregisterSurface(33);
}

// The idle view above is composited from the frame it already has, not from
// one that lands while that present waits out the flip: that frame belongs to
// the present it nudged, which can put it on a plane. Taking it instead left
// that present nothing new either, and a client on a Pi 5 composited on every
// present at 40 fps, none of them on a plane.
TEST_F(DrmBackendVkmsScene, AnIdleViewIsCompositedFromTheFrameItHas) {
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF1E7A46u));

  auto view = std::make_shared<FakeDmabufPlatformView>(34, buffer, 3);
  view->set_offers_unimportable_frame(true);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(34, view);

  ASSERT_TRUE(PresentPlatformView(34));
  EXPECT_EQ(view->held_presents(), 0)
      << "a present with a new frame held the view to its old one";

  // No new frame: composited, held to what it has.
  ASSERT_TRUE(PresentPlatformView(34));
  EXPECT_EQ(view->held_presents(), 1);
  EXPECT_FALSE(view->gl_held()) << "the hold outlived its present";

  compositor_->UnregisterSurface(34);
}

// The release fence (#513). Same displacement as the case above, but the
// producer also gets the OUT_FENCE of the commit that displaced its buffer --
// the precise "off the plane" edge, where the deferred eventfd only
// approximates it by holding the release until WaitForPendingFlip says the flip
// landed.
//
// drm-cxx already requested and imported this fence: ExternalDmaBufPool opts in
// via wants_release_fence(), so LayerScene attaches OUT_FENCE_PTR on every real
// commit with such a source and stamps the result per buffer. The shell was
// taking the key from on_release and dropping the fence on the floor.
TEST_F(DrmBackendVkmsScene, ADisplacedSlotComesBackWithItsReleaseFence) {
  GbmSolidBuffer first;
  GbmSolidBuffer second;
  ASSERT_TRUE(first.Create(backend_->device().fd(), content_w_, content_h_,
                           0xFF1E7A46u));
  ASSERT_TRUE(second.Create(backend_->device().fd(), content_w_, content_h_,
                            0xFF7A1E46u));

  auto view = std::make_shared<FakeDmabufPlatformView>(25, first, 0);
  layer_w_ = content_w_;
  layer_h_ = content_h_;
  compositor_->RegisterSurface(25, view);

  ASSERT_TRUE(PresentPlatformView(25));
  EXPECT_EQ(view->release_fences(), 0)
      << "a fence before anything was displaced";

  view->Submit(&second, 1);
  ASSERT_TRUE(PumpUntil(25, [&] { return !view->released().empty(); }))
      << "slot 0 never came back";

  if (view->release_fences() == 0) {
    GTEST_SKIP() << "no release fence published; this CRTC has no "
                    "OUT_FENCE_PTR, so the eventfd edge is the only signal";
  }

  // Published with the release, not instead of it: a producer that cannot wait
  // on a sync_file still has the eventfd, so both must arrive.
  EXPECT_EQ(view->released().front(), 0u) << "the eventfd release still fires";
  EXPECT_TRUE(view->ReleaseFenceSignals(1000))
      << "the fence handed back never signaled, so a producer waiting on it "
         "would stall rather than reuse the slot";

  compositor_->UnregisterSurface(25);
}

// ─── Explicit-sync acquire (#513) ────────────────────────────────────────
//
// EglDmabufImporter::WaitAcquireFence is what lets an EGL backend advertise
// IhsPvCapabilities::explicit_sync: instead of the raster thread blocking in
// poll() on the producer's sync_file, the fence is handed to the GL driver and
// the GPU waits.
//
// These drive the importer directly. The call site is IhsPluginView::
// GetGlTextureName, which lives in an anonymous namespace in the view host and
// cannot be constructed from a test -- the FakePlatformView above implements
// GetGlTextureName itself, so presenting it never reaches the real wait. What
// is coverable, and what is worth covering, is the fd-ownership contract:
// eglCreateSyncKHR takes the fd on success and leaves it on failure, and the
// caller closes it only in the second case. Get that backwards and the GL path
// either double-closes or leaks one fd per frame.
class EglAcquireFence : public DrmBackendVkmsBase {
 protected:
  void Configure(DrmConfig& cfg) override {
    cfg.compositor = drm_config::Compositor::kGl;
    content_w_ = card_.mode_w;
    content_h_ = card_.mode_h;
  }

  // Mint a real sync_file by fencing GL work on the backend's own context,
  // which is the same shape a producer's acquire fence has.
  [[nodiscard]] int MintFence() {
    auto create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        eglGetProcAddress("eglCreateSyncKHR"));
    auto destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        eglGetProcAddress("eglDestroySyncKHR"));
    auto dup_fd = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (create == nullptr || destroy == nullptr || dup_fd == nullptr) {
      return -1;
    }
    const EGLDisplay dpy = backend_->egl_display();
    glClear(GL_COLOR_BUFFER_BIT);
    EGLSyncKHR s = create(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (s == EGL_NO_SYNC_KHR) {
      return -1;
    }
    glFlush();
    const int fd = dup_fd(dpy, s);
    destroy(dpy, s);
    return fd;
  }

  static bool FdOpen(int fd) {
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
  }
};

// On success EGL owns the fence: the caller must not close it, and after the
// sync is destroyed the fd is gone. This is the branch the GL path takes on any
// driver with native fence sync, so a leak here is a leak every frame.
TEST_F(EglAcquireFence, ASuccessfulWaitConsumesTheProducersFence) {
  ASSERT_TRUE(backend_->MakeCurrent());
  EglDmabufImporter importer;
  ASSERT_TRUE(importer.Init(backend_->egl_display()));
  if (!importer.has_native_fence_sync()) {
    GTEST_SKIP() << "display has no EGL_ANDROID_native_fence_sync; the GL path "
                    "falls back to the CPU wait here";
  }

  const int fence = MintFence();
  ASSERT_GE(fence, 0) << "could not mint a sync_file to stand in for the "
                         "producer's acquire fence";
  ASSERT_TRUE(FdOpen(fence));

  EXPECT_TRUE(importer.WaitAcquireFence(fence));
  EXPECT_FALSE(FdOpen(fence))
      << "EGL took ownership of the fence, so it must be closed -- the caller "
         "skipping its close() depends on exactly this";
}

// The importer reports the capability separately from ready(): a display that
// imports dma-bufs but cannot wait on a sync_file still imports, and is what
// leaves IhsPvCapabilities::explicit_sync at 0.
TEST_F(EglAcquireFence, ImportStaysAvailableIndependentOfFenceSync) {
  ASSERT_TRUE(backend_->MakeCurrent());
  EglDmabufImporter importer;
  ASSERT_TRUE(importer.Init(backend_->egl_display()));
  EXPECT_TRUE(importer.ready())
      << "dma-buf import is what Init's return value is about";
}

// A negative fd is the implicit-sync submit -- the producer stalled
// synchronously and handed over no fence. Nothing to wait on, and nothing to
// close, so the caller must not be told the fd was consumed.
TEST_F(EglAcquireFence, NoFenceIsNotConsumed) {
  ASSERT_TRUE(backend_->MakeCurrent());
  EglDmabufImporter importer;
  ASSERT_TRUE(importer.Init(backend_->egl_display()));
  EXPECT_FALSE(importer.WaitAcquireFence(-1));
}

// ─── The real ihs_pv host path (#602) ────────────────────────────────────
//
// Everything above drives ICompositorSurface directly, so it exercises the
// compositor and never HostSubmit. That is how #593 -- one leaked sync_file per
// explicit-sync submit -- reached a release: it lives in HostSubmit, and
// InstallPlatformViewHost is called from flutter_desktop.cc and nowhere else.
//
// The host needs less of an engine than it looks. BackendOf walks
// state->view_controller->view->GetBackend(), and InstallPlatformViewHost wants
// a non-null platform_view_registry; nothing on the install path touches
// flutter_engine. So a hand-built state over a FlutterView is enough, and
// FlutterView's constructor builds the backend itself (Backend::Create) --
// which is why this fixture does not extend DrmBackendVkmsBase: that one
// creates its own DrmBackend, and two cannot hold DRM master on one card.
//
// Initialize() is deliberately not called. It wants a live engine; the
// constructor alone is what stands the backend up.
class PvHostVkms : public ::testing::Test {
 protected:
  void SetUp() override {
    card_ = FindVkms();
    if (!card_.ok()) {
      GTEST_SKIP() << "no connected vkms card (sudo modprobe vkms)";
    }
    display_ = std::make_shared<DrmDisplay>(0, 0, 0.0, card_.path,
                                            /*no_seat=*/true);
    if (display_->SharedDevice() == nullptr) {
      GTEST_SKIP() << "no DRM master on " << card_.path;
    }

    // Production populates the registry on the way into App; nothing does in a
    // test, and an empty registry resolves to key='' rather than failing
    // loudly at the point of the mistake. Idempotent, so re-running per case is
    // fine.
    RegisterCompiledBackends(backend::BackendRegistry::Instance());

    Configuration::Config cfg{};
    cfg.view.backend = "drm-kms-egl";
    cfg.view.drm_device = card_.path;
    cfg.view.width = card_.mode_w;
    cfg.view.height = card_.mode_h;
    cfg.view.drm_no_seat = true;
    Tune(cfg);

    view_ = std::make_unique<FlutterView>(cfg, 0, "pv-host-test", display_);
    ASSERT_NE(view_->GetBackend(), nullptr)
        << "FlutterView built no backend for drm-kms-egl on " << card_.path;

    controller_.view = view_.get();
    state_.view_controller = &controller_;
    state_.platform_view_registry =
        std::make_unique<PlatformViewRegistry>(&state_);

    InstallPlatformViewHost(&state_);
    installed_ = true;
  }

  void TearDown() override {
    // The host is process-global. Leaving it installed would point a later case
    // at a destroyed engine state.
    if (installed_) {
      ihs_pv_unregister_factory(kViewType);
      ihs_pv_set_host(nullptr);
      installed_ = false;
    }
    state_.platform_view_registry.reset();
    view_.reset();
    display_.reset();
  }

  // A subclass's say in the configuration, before the backend is built.
  virtual void Tune(Configuration::Config& /*cfg*/) {}

  static constexpr const char* kViewType = "views/fd-audit";

  VkmsCard card_;
  std::shared_ptr<DrmDisplay> display_;
  std::unique_ptr<FlutterView> view_;
  FlutterDesktopViewControllerState controller_{};
  FlutterDesktopEngineState state_{};
  bool installed_{false};
};

// The producer the factory hands back: it keeps the IhsPlatformView so the case
// can submit against it, and counts the callbacks the registry drives.
struct FakeProducer {
  IhsPlatformView* view{nullptr};
  // Atomic: presented reads it from the display thread.
  std::atomic<int> disposed{0};
  // Reports that arrived once dispose had started.
  std::atomic<int> late_reports{0};

  // IhsPvCallbacks::presented, which arrives on the display thread.
  struct Presented {
    uint64_t seq;
    uint64_t ust_ns;
    uint32_t refresh_ns;
    uint64_t msc;
    uint32_t flags;
  };
  std::mutex mu;
  std::vector<Presented> presented;

  // IhsPvCallbacks::scanout_hint, also on the display thread.
  struct ScanoutHint {
    uint32_t layer_id;
    uint64_t dev;
    std::vector<IhsFormatModifier> formats;
  };
  std::vector<ScanoutHint> hints;

  std::vector<ScanoutHint> TakeHints() {
    const std::lock_guard<std::mutex> lock(mu);
    std::vector<ScanoutHint> out;
    out.swap(hints);
    return out;
  }

  std::vector<Presented> TakePresented() {
    const std::lock_guard<std::mutex> lock(mu);
    std::vector<Presented> out;
    out.swap(presented);
    return out;
  }
};

int fake_factory(const IhsPvCreateInfo* /*info*/,
                 void* factory_user_data,
                 IhsPlatformView* view,
                 IhsPvCallbacks* out_callbacks,
                 void** out_user_data) {
  auto* p = static_cast<FakeProducer*>(factory_user_data);
  p->view = view;
  out_callbacks->struct_size = sizeof(*out_callbacks);
  out_callbacks->dispose = [](void* u) {
    static_cast<FakeProducer*>(u)->disposed++;
  };
  out_callbacks->presented = [](void* u, uint64_t seq, uint64_t ust_ns,
                                uint32_t refresh_ns, uint64_t msc,
                                uint32_t flags) {
    auto* producer = static_cast<FakeProducer*>(u);
    if (producer->disposed.load() != 0) {
      producer->late_reports.fetch_add(1);
    }
    const std::lock_guard<std::mutex> lock(producer->mu);
    producer->presented.push_back({seq, ust_ns, refresh_ns, msc, flags});
  };
  out_callbacks->scanout_hint = [](void* u, uint32_t layer_id, uint64_t dev,
                                   const IhsFormatModifier* formats,
                                   size_t count) {
    auto* producer = static_cast<FakeProducer*>(u);
    const std::lock_guard<std::mutex> lock(producer->mu);
    producer->hints.push_back(
        {layer_id, dev,
         std::vector<IhsFormatModifier>(formats, formats + count)});
  };
  *out_user_data = p;
  return IHS_PV_OK;
}

// The explicit-sync submit loop must not cost an fd per frame.
//
// This is #593's shape. HandBackReleaseFence stores a dup of the compositor's
// release fence in out_release_fence_fd and HandBackReleaseEventfd then
// supersedes it; overwriting without closing leaked one sync_file per submit.
//
// Both of its preconditions have to be armed or the leaking line never runs:
// the submit must carry an acquire fence, and the view must already hold a
// release fence. The first is minted here; the second needs the plane path to
// have presented the view at least once, which is why this presents before it
// measures. An earlier draft of this test did neither, passed against a
// deliberately reintroduced leak, and would have been worthless.
TEST_F(PvHostVkms, ExplicitSyncSubmitLoopDoesNotLeakFds) {
  auto* drm = dynamic_cast<DrmBackend*>(view_->GetBackend());
  ASSERT_NE(drm, nullptr) << "not a DrmBackend";
  DrmCompositor* comp = drm->compositor();
  ASSERT_NE(comp, nullptr) << "built without BUILD_COMPOSITOR";

  FakeProducer producer;
  ASSERT_EQ(ihs_pv_register_factory(kViewType, &fake_factory, &producer),
            IHS_PV_OK);

  PlatformViewRegistry::CreateRequest req{};
  req.id = 1;
  req.view_type = kViewType;
  req.width = 64;
  req.height = 64;
  ASSERT_TRUE(state_.platform_view_registry->CreateViaFactory(req));
  ASSERT_NE(producer.view, nullptr) << "the factory was never invoked";

  IhsPvRequirements reqs{};
  reqs.struct_size = sizeof(reqs);
  reqs.kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_DRM_PLANE;
  reqs.sync = IHS_PV_SYNC_EXPLICIT_PREFERRED;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(producer.view, &reqs, &grant), IHS_PV_OK);
  if (grant.granted_kind == IHS_PV_KIND_SOFTWARE_SHM) {
    GTEST_SKIP() << "negotiated down to the software floor; HostSubmit's EGL "
                    "branch is not reachable on this display";
  }

  ASSERT_TRUE(drm->MakeCurrent());

  // The card fd off the display: GetBackend() hands back the base Backend,
  // which exposes no device().
  GbmSolidBuffer buffer;
  ASSERT_TRUE(
      buffer.Create(display_->SharedDevice()->fd(), 64, 64, 0xFF1E7A46u))
      << "could not allocate a dma-buf on the card";

  // A real producer acquire fence: fence some GL work and export the sync_file.
  auto mint_acquire = [&]() -> int {
    auto create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        eglGetProcAddress("eglCreateSyncKHR"));
    auto destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        eglGetProcAddress("eglDestroySyncKHR"));
    auto dup_fd = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (create == nullptr || destroy == nullptr || dup_fd == nullptr) {
      return -1;
    }
    const EGLDisplay dpy = drm->egl_display();
    glClear(GL_COLOR_BUFFER_BIT);
    EGLSyncKHR s = create(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (s == EGL_NO_SYNC_KHR) {
      return -1;
    }
    glFlush();
    const int fd = dup_fd(dpy, s);
    destroy(dpy, s);
    return fd;
  };

  auto submit = [&](uint32_t slot, int acquire_fd, int* out_release) {
    IhsFrame frame{};
    frame.struct_size = sizeof(frame);
    frame.format.fourcc = DRM_FORMAT_XRGB8888;
    frame.format.modifier = DRM_FORMAT_MOD_LINEAR;
    frame.width = 64;
    frame.height = 64;
    frame.plane_count = 1;
    frame.plane_fd[0] = buffer.ExportFd();
    frame.plane_offset[0] = 0;
    frame.plane_stride[0] = buffer.stride();
    frame.buffer_id = slot;
    // Plane fds are not closed here: ihs_pv_submit consumes them whatever it
    // returns (see FD OWNERSHIP on IhsFrame). Closing them is a double close,
    // which does not report itself -- it lands several submits later on an
    // unrelated fd that reused the number, and surfaces as the release write
    // failing with EINVAL. The first draft of this test did exactly that.
    return ihs_pv_submit(producer.view, &frame, acquire_fd, out_release);
  };

  auto present = [&]() {
    FlutterPlatformView pv{};
    pv.struct_size = sizeof(FlutterPlatformView);
    pv.identifier = 1;
    FlutterLayer layer{};
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypePlatformView;
    layer.platform_view = &pv;
    layer.offset = FlutterPoint{0.0, 0.0};
    layer.size = FlutterSize{64.0, 64.0};
    const FlutterLayer* layers[] = {&layer};
    return comp->PresentLayers(layers, 1);
  };

  // Prime the release fence. The plane path hands one back through
  // SetReleaseFenceFd only after a commit has displaced a buffer, so this needs
  // a frame, a present that places it, and a second present whose flip wait
  // fires the deferred release.
  int rel = -1;
  ASSERT_EQ(submit(0, -1, &rel), IHS_PV_OK);
  if (rel >= 0) {
    ::close(rel);
  }
  present();
  ASSERT_EQ(submit(1, -1, &rel), IHS_PV_OK);
  if (rel >= 0) {
    ::close(rel);
  }
  present();

  // A real producer holds the release fd until it wants the slot back, so this
  // does too: the host signals a slot's eventfd when the next submit for that
  // buffer_id retires the stale entry, and that readability is the only outward
  // sign the host still holds a valid fd. A double close leaves the fd count
  // flat and shows up nowhere else.
  int held[2] = {-1, -1};
  int release_failures = 0;
  const auto submit_explicit = [&](uint32_t slot) {
    const int acquire = mint_acquire();
    int release_fd = -1;
    const int rc = submit(slot, acquire, &release_fd);
    if (held[slot] >= 0) {
      pollfd pfd{held[slot], POLLIN, 0};
      if (::poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLIN) == 0) {
        ++release_failures;
      }
      ::close(held[slot]);
    }
    held[slot] = release_fd;
    return rc;
  };

  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(submit_explicit(static_cast<uint32_t>(i % 2)), IHS_PV_OK)
        << "warm-up submit " << i;
    present();
  }
  const int before = CountOpenFds();
  ASSERT_GT(before, 0) << "could not read /proc/self/fd";

  for (int i = 0; i < 64; ++i) {
    ASSERT_EQ(submit_explicit(static_cast<uint32_t>(i % 2)), IHS_PV_OK)
        << "submit " << i;
    present();
  }
  const int after = CountOpenFds();

  EXPECT_LE(after - before, 4)
      << "open fds grew from " << before << " to " << after
      << " over 64 explicit-sync submits; the submit path is leaking about one "
         "per frame (#593)";

  EXPECT_EQ(release_failures, 0)
      << "a release eventfd never signaled; the host could not write to an fd "
         "it owns, which means one was closed underneath it";

  for (int& fd : held) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  EXPECT_TRUE(state_.platform_view_registry->Dispose(1, false));
}

// What a plane is showing, read back from KMS rather than from what the
// compositor believes it committed.
struct PlaneState {
  uint32_t fb_id{0};
  uint64_t src_x{0};
  uint64_t src_y{0};
  uint64_t src_w{0};
  uint64_t src_h{0};
  int64_t crtc_x{0};
  int64_t crtc_y{0};
  uint64_t crtc_w{0};
  uint64_t crtc_h{0};
  uint64_t rotation{DRM_MODE_ROTATE_0};
  bool has_rotation{false};
};

// Every plane on @p path with a framebuffer attached.
std::vector<PlaneState> ActivePlanes(const std::string& path) {
  std::vector<PlaneState> out;
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return out;
  }
  // FB_ID, SRC_* and CRTC_* are atomic properties, listed only to a client
  // that has asked for atomic.
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
  if (drmModePlaneRes* pres = drmModeGetPlaneResources(fd); pres != nullptr) {
    for (uint32_t i = 0; i < pres->count_planes; ++i) {
      drmModeObjectProperties* props = drmModeObjectGetProperties(
          fd, pres->planes[i], DRM_MODE_OBJECT_PLANE);
      if (props == nullptr) {
        continue;
      }
      PlaneState st;
      for (uint32_t p = 0; p < props->count_props; ++p) {
        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[p]);
        if (prop == nullptr) {
          continue;
        }
        const std::string name = prop->name;
        const uint64_t v = props->prop_values[p];
        if (name == "FB_ID") {
          st.fb_id = static_cast<uint32_t>(v);
        } else if (name == "SRC_X") {
          st.src_x = v;
        } else if (name == "SRC_Y") {
          st.src_y = v;
        } else if (name == "SRC_W") {
          st.src_w = v;
        } else if (name == "SRC_H") {
          st.src_h = v;
        } else if (name == "CRTC_X") {
          st.crtc_x = static_cast<int64_t>(v);
        } else if (name == "CRTC_Y") {
          st.crtc_y = static_cast<int64_t>(v);
        } else if (name == "CRTC_W") {
          st.crtc_w = v;
        } else if (name == "CRTC_H") {
          st.crtc_h = v;
        } else if (name == "rotation") {
          st.rotation = v;
          st.has_rotation = true;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
      if (st.fb_id != 0) {
        out.push_back(st);
      }
    }
    drmModeFreePlaneResources(pres);
  }
  ::close(fd);
  return out;
}

// Whether any plane on @p path can rotate.
bool AnyPlaneRotates(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  bool found = false;
  if (drmModePlaneRes* pres = drmModeGetPlaneResources(fd); pres != nullptr) {
    for (uint32_t i = 0; i < pres->count_planes && !found; ++i) {
      drmModeObjectProperties* props = drmModeObjectGetProperties(
          fd, pres->planes[i], DRM_MODE_OBJECT_PLANE);
      if (props == nullptr) {
        continue;
      }
      for (uint32_t p = 0; p < props->count_props && !found; ++p) {
        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[p]);
        if (prop != nullptr) {
          found = std::string(prop->name) == "rotation";
          drmModeFreeProperty(prop);
        }
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlaneResources(pres);
  }
  ::close(fd);
  return found;
}

// The view host on the plane compositor, pinned rather than left to the driver
// probe, with the helpers the layer cases share.
class PvHostVkmsPlanes : public PvHostVkms {
 protected:
  // The present path the case runs on.
  [[nodiscard]] virtual const char* CompositorMode() const { return "planes"; }

  void SetUp() override {
    const VkmsCard card = FindVkms();
    if (card.ok() && card.overlays < 2 &&
        std::string(CompositorMode()) == "planes") {
      GTEST_SKIP() << "vkms needs two overlay planes for a view's layers "
                      "(modprobe vkms enable_overlay=1)";
    }
    PvHostVkms::SetUp();
    if (IsSkipped()) {
      return;
    }
    auto* drm = dynamic_cast<DrmBackend*>(view_->GetBackend());
    ASSERT_NE(drm, nullptr) << "not a DrmBackend";
    comp_ = drm->compositor();
    ASSERT_NE(comp_, nullptr) << "built without BUILD_COMPOSITOR";
    ASSERT_TRUE(drm->MakeCurrent());

    ASSERT_EQ(ihs_pv_register_factory(kViewType, &fake_factory, &producer_),
              IHS_PV_OK);
    PlatformViewRegistry::CreateRequest req{};
    req.id = 1;
    req.view_type = kViewType;
    req.width = kViewW;
    req.height = kViewH;
    ASSERT_TRUE(state_.platform_view_registry->CreateViaFactory(req));
    ASSERT_NE(producer_.view, nullptr) << "the factory was never invoked";
  }

  void TearDown() override {
    if (producer_.view != nullptr) {
      state_.platform_view_registry->Dispose(1, false);
      producer_.view = nullptr;
    }
    PvHostVkms::TearDown();
  }

  void Tune(Configuration::Config& cfg) override {
    cfg.view.drm_compositor = CompositorMode();
  }

  // Submit @p buffer whole as one layer, under @p seq.
  int SubmitSeq(const GbmSolidBuffer& buffer,
                const uint32_t buffer_id,
                const uint64_t seq) const {
    IhsFrame f = FrameOf(buffer, buffer_id);
    IhsLayer layer = LayerOf(&f, 1);
    int release = -1;
    const int rc =
        ihs_pv_submit_layers(producer_.view, &layer, 1, seq, &release);
    if (release >= 0) {
      ::close(release);
    }
    return rc;
  }

  // SubmitSeq, keeping the release fence in *@p release.
  int SubmitKeepingRelease(const GbmSolidBuffer& buffer,
                           const uint32_t buffer_id,
                           const uint64_t seq,
                           int* release) const {
    IhsFrame f = FrameOf(buffer, buffer_id);
    IhsLayer layer = LayerOf(&f, 1);
    return ihs_pv_submit_layers(producer_.view, &layer, 1, seq, release);
  }

  // A frame on a plane when its view leaves the scene is released once the
  // scene retires the view's layer, whether the view is disposed before the
  // frames without it are presented or after. The release rides the scene's
  // later commits, which present the UI alone.
  void ExpectAFrameOnAPlaneReleasedOnceTheViewLeaves(const bool dispose_first) {
    const int fd = display_->SharedDevice()->fd();
    GbmSolidBuffer a;
    GbmSolidBuffer b;
    ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
    ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
    ASSERT_EQ(SubmitSeq(a, 0, 1), IHS_PV_OK);
    ASSERT_EQ(PresentUntilReported(1).size(), 1U);

    int release = -1;
    ASSERT_EQ(SubmitKeepingRelease(b, 1, 2, &release), IHS_PV_OK);
    ASSERT_GE(release, 0) << "no release fence for the frame";
    ASSERT_TRUE(Present());
    ASSERT_TRUE(Present());
    ASSERT_FALSE(Readable(release)) << "released while still on screen";

    FlutterBackingStoreConfig cfg{};
    cfg.struct_size = sizeof(FlutterBackingStoreConfig);
    cfg.size = FlutterSize{static_cast<double>(card_.mode_w),
                           static_cast<double>(card_.mode_h)};
    FlutterBackingStore bs{};
    ASSERT_TRUE(comp_->CreateBackingStore(&cfg, &bs));
    FlutterLayer ui{};
    ui.struct_size = sizeof(FlutterLayer);
    ui.type = kFlutterLayerContentTypeBackingStore;
    ui.backing_store = &bs;
    ui.size = cfg.size;
    const FlutterLayer* layers[] = {&ui};

    if (dispose_first) {
      ASSERT_TRUE(state_.platform_view_registry->Dispose(1, false));
      producer_.view = nullptr;
    }
    bool fired = false;
    for (int i = 0; i < 10 && !fired; ++i) {
      ASSERT_TRUE(comp_->PresentLayers(layers, 1)) << "present " << i;
      fired = Readable(release);
      if (i == 0 && !dispose_first) {
        ASSERT_TRUE(state_.platform_view_registry->Dispose(1, false));
        producer_.view = nullptr;
      }
    }
    ::close(release);
    EXPECT_TRUE(fired) << "the frame on the plane was never released";
    comp_->CollectBackingStore(&bs);
  }

  // A frame the compositor still holds when its view is disposed is released
  // once the compositor lets go of it. Its release fence used to be closed at
  // dispose without firing, so a producer that keeps its buffers past the
  // view waited on it forever.
  void ExpectAHeldFrameReleasedAfterDispose() {
    const int fd = display_->SharedDevice()->fd();
    GbmSolidBuffer a;
    GbmSolidBuffer b;
    ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
    ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
    ASSERT_EQ(SubmitSeq(a, 0, 1), IHS_PV_OK);
    ASSERT_EQ(PresentUntilReported(1).size(), 1U);

    int release = -1;
    ASSERT_EQ(SubmitKeepingRelease(b, 1, 2, &release), IHS_PV_OK);
    ASSERT_GE(release, 0) << "no release fence for the frame";
    ASSERT_TRUE(Present());
    ASSERT_TRUE(state_.platform_view_registry->Dispose(1, false));
    producer_.view = nullptr;

    bool fired = Readable(release);
    for (int i = 0; i < 10 && !fired; ++i) {
      Present();
      fired = Readable(release);
    }
    ::close(release);
    EXPECT_TRUE(fired) << "the frame held at dispose was never released";
  }

  // Present until the producer has been told about @p want frames, or give up.
  std::vector<FakeProducer::Presented> PresentUntilReported(size_t want,
                                                            int max = 10) {
    std::vector<FakeProducer::Presented> got;
    for (int i = 0; i < max && got.size() < want; ++i) {
      if (!Present()) {
        break;
      }
      for (const auto& p : producer_.TakePresented()) {
        got.push_back(p);
      }
    }
    return got;
  }

  // A frame for @p buffer, which ihs_pv_submit_layers consumes.
  static IhsFrame FrameOf(const GbmSolidBuffer& buffer, uint32_t buffer_id) {
    IhsFrame frame{};
    frame.struct_size = sizeof(frame);
    frame.format.fourcc = DRM_FORMAT_XRGB8888;
    frame.format.modifier = DRM_FORMAT_MOD_LINEAR;
    frame.width = buffer.width();
    frame.height = buffer.height();
    frame.plane_count = 1;
    frame.plane_fd[0] = buffer.ExportFd();
    frame.plane_stride[0] = buffer.stride();
    frame.buffer_id = buffer_id;
    return frame;
  }

  static IhsLayer LayerOf(const IhsFrame* frame, uint32_t layer_id) {
    IhsLayer layer{};
    layer.struct_size = sizeof(layer);
    layer.frame = frame;
    layer.acquire_fence_fd = -1;
    layer.layer_id = layer_id;
    return layer;
  }

  bool Present() {
    FlutterPlatformView pv{};
    pv.struct_size = sizeof(FlutterPlatformView);
    pv.identifier = 1;
    FlutterLayer layer{};
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypePlatformView;
    layer.platform_view = &pv;
    layer.offset = FlutterPoint{0.0, 0.0};
    layer.size =
        FlutterSize{static_cast<double>(kViewW), static_cast<double>(kViewH)};
    const FlutterLayer* layers[] = {&layer};
    return comp_->PresentLayers(layers, 1);
  }

  static bool Readable(int fd) {
    pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0;
  }

  static constexpr uint32_t kViewW = 128;
  static constexpr uint32_t kViewH = 128;
  DrmCompositor* comp_{nullptr};
  FakeProducer producer_;
};

// An import-granted view is placed on a plane like any other dma-buf, so the
// plane accessor reports the plane it is on rather than 0. It used to return 0
// for anything but a DRM_PLANE grant, which answered "what kind was granted"
// when the caller asked "where did my frame land" -- and an import grant is
// scanned out routinely, so the answer was wrong rather than merely narrow
// (ivi-homescreen#704).
TEST_F(PvHostVkmsPlanes, AnImportGrantReportsThePlaneItIsScannedOutOn) {
  IhsPvRequirements reqs{};
  reqs.struct_size = sizeof(reqs);
  // Import only: no DRM_PLANE in the mask, so the grant cannot be that.
  reqs.kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
  reqs.sync = IHS_PV_SYNC_IMPLICIT;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(producer_.view, &reqs, &grant), IHS_PV_OK);
  ASSERT_EQ(grant.granted_kind, IHS_PV_KIND_TEXTURE_DMABUF_IMPORT)
      << "this case is about what an import grant reports";

  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer buffer;
  ASSERT_TRUE(buffer.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_EQ(SubmitSeq(buffer, 3, 1), IHS_PV_OK);
  ASSERT_TRUE(Present());

  // KMS first: the view really is on a plane. Without this the accessor could
  // agree with a compositor that never placed anything.
  const std::vector<PlaneState> planes = ActivePlanes(card_.path);
  const auto placed =
      std::find_if(planes.begin(), planes.end(), [](const PlaneState& p) {
        return p.crtc_w == kViewW && p.crtc_h == kViewH;
      });
  ASSERT_NE(placed, planes.end())
      << "the import-granted view was not scanned out on a plane, so there is "
         "nothing for the accessor to report";
  EXPECT_NE(ihs_pv_grant_drm_plane_id(producer_.view), 0U)
      << "on a plane per KMS, but the accessor still reports 0";
}

// A bottom-up frame is never scanned out, even one that fits a plane: a plane
// would show it upside down. It is composited instead, flipped as it is
// sampled, and the producer is not hinted toward a format, since none would
// put it on a plane. The same buffer top-first goes back to a plane.
TEST_F(PvHostVkmsPlanes, ABottomUpFrameIsCompositedNotScannedOut) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
  const auto on_plane = [this] {
    const std::vector<PlaneState> planes = ActivePlanes(card_.path);
    return std::any_of(planes.begin(), planes.end(), [](const PlaneState& p) {
      return p.crtc_w == kViewW && p.crtc_h == kViewH;
    });
  };
  const auto submit = [this](const GbmSolidBuffer& buffer, uint32_t buffer_id,
                             uint8_t flags, uint64_t seq) {
    IhsFrame f = FrameOf(buffer, buffer_id);
    f.flags = flags;
    IhsLayer layer = LayerOf(&f, 1);
    int release = -1;
    const int rc =
        ihs_pv_submit_layers(producer_.view, &layer, 1, seq, &release);
    if (release >= 0) {
      ::close(release);
    }
    return rc;
  };

  ASSERT_EQ(submit(a, 0, IHS_PV_FRAME_BOTTOM_UP, 1), IHS_PV_OK);
  auto got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the bottom-up frame was never reported";
  EXPECT_EQ(got[0].seq, 1U);
  EXPECT_EQ(got[0].flags & IHS_PV_PRESENTED_ZERO_COPY, 0U)
      << "reported zero-copy, so it was scanned out";
  EXPECT_FALSE(on_plane()) << "a bottom-up frame is on a plane";
  EXPECT_EQ(ihs_pv_grant_drm_plane_id(producer_.view), 0U);
  EXPECT_TRUE(producer_.TakeHints().empty())
      << "hinted a format for a frame no format puts on a plane";

  ASSERT_EQ(submit(b, 1, 0, 2), IHS_PV_OK);
  got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the top-first frame was never reported";
  EXPECT_EQ(got[0].seq, 2U);
  EXPECT_NE(got[0].flags & IHS_PV_PRESENTED_ZERO_COPY, 0U);
  EXPECT_TRUE(on_plane()) << "the top-first frame did not go back to a plane";
}

// The other half of the rule: a kind that cannot reach a plane reports 0. shm
// is CPU-filled, so there is no plane to name whatever the compositor does.
TEST_F(PvHostVkmsPlanes, ASoftwareGrantReportsNoPlane) {
  IhsPvRequirements reqs{};
  reqs.struct_size = sizeof(reqs);
  reqs.kinds = IHS_PV_KIND_SOFTWARE_SHM;
  reqs.sync = IHS_PV_SYNC_IMPLICIT;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ASSERT_EQ(ihs_pv_negotiate(producer_.view, &reqs, &grant), IHS_PV_OK);
  ASSERT_EQ(grant.granted_kind, IHS_PV_KIND_SOFTWARE_SHM);
  EXPECT_EQ(ihs_pv_grant_drm_plane_id(producer_.view), 0U);
}

// Each layer of a view is scanned out on a plane of its own, with its own
// crop, place and rotation -- read back from KMS, not from the compositor.
TEST_F(PvHostVkmsPlanes, EachLayerOfAViewGetsItsOwnPlane) {
  if (!AnyPlaneRotates(card_.path)) {
    GTEST_SKIP() << "no plane on this vkms has a rotation property";
  }
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer bottom;
  GbmSolidBuffer top;
  ASSERT_TRUE(bottom.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(top.Create(fd, 64, 32, 0xFF7A1E46u));

  IhsFrame f0 = FrameOf(bottom, 0);
  IhsFrame f1 = FrameOf(top, 1);
  IhsLayer layers[2] = {LayerOf(&f0, 10), LayerOf(&f1, 11)};
  // The top layer shows a 48x24 crop of its buffer at (4, 2), rotated: at 90
  // degrees that is a 24x48 rect, placed at (16, 8) within the view.
  layers[1].src_x = 4 << 16;
  layers[1].src_y = 2 << 16;
  layers[1].src_w = 48U << 16U;
  layers[1].src_h = 24U << 16U;
  layers[1].dst_x = 16;
  layers[1].dst_y = 8;
  layers[1].dst_w = 24;
  layers[1].dst_h = 48;
  layers[1].transform = IHS_TRANSFORM_90;
  int release[2] = {-1, -1};
  ASSERT_EQ(ihs_pv_submit_layers(producer_.view, layers, 2, 1, release),
            IHS_PV_OK);
  for (int& r : release) {
    if (r >= 0) {
      ::close(r);
    }
  }
  ASSERT_TRUE(Present());

  // Three planes: the view's two layers, plus the shell's background under
  // them, full mode on the primary. The frame has no backing store, so the
  // shell puts one there; the primary used to keep whatever the modeset left,
  // until the scene started turning off, on its first commit, the planes it
  // did not light.
  // Assert what is on each plane rather than a bare count, so the next
  // allocator change that adds or merges a plane fails on content and not on
  // arithmetic.
  const std::vector<PlaneState> planes = ActivePlanes(card_.path);
  const auto shell =
      std::find_if(planes.begin(), planes.end(), [this](const PlaneState& p) {
        return p.crtc_w == card_.mode_w && p.crtc_h == card_.mode_h;
      });
  ASSERT_NE(shell, planes.end())
      << "the shell's own frame is not on a plane; the view's layers are not "
         "the only thing that has to reach the display";
  ASSERT_EQ(planes.size(), 3U)
      << "expected the view's two layers plus the shell's frame, nothing else";
  const auto whole =
      std::find_if(planes.begin(), planes.end(), [](const PlaneState& p) {
        return p.crtc_w == kViewW && p.crtc_h == kViewH;
      });
  const auto placed = std::find_if(
      planes.begin(), planes.end(),
      [](const PlaneState& p) { return p.crtc_w == 24 && p.crtc_h == 48; });
  ASSERT_NE(whole, planes.end()) << "the bottom layer is not the whole view";
  ASSERT_NE(placed, planes.end()) << "the top layer is not where it was put";
  EXPECT_EQ(whole->src_w, uint64_t{kViewW} << 16U);
  EXPECT_EQ(whole->src_h, uint64_t{kViewH} << 16U);
  EXPECT_EQ(placed->crtc_x, 16);
  EXPECT_EQ(placed->crtc_y, 8);
  EXPECT_EQ(placed->src_x, uint64_t{4} << 16U);
  EXPECT_EQ(placed->src_y, uint64_t{2} << 16U);
  EXPECT_EQ(placed->src_w, uint64_t{48} << 16U);
  EXPECT_EQ(placed->src_h, uint64_t{24} << 16U);
  // Undoing a producer's counter-clockwise quarter turn is a clockwise one.
  EXPECT_EQ(placed->rotation, static_cast<uint64_t>(DRM_MODE_ROTATE_270));
}

// A retired id submitted again for new memory scans out that memory -- the
// plane path's framebuffer cache is keyed by generation, not by id alone --
// and a late release of the old memory does not hand the new frame back while
// it is on screen.
TEST_F(PvHostVkmsPlanes, AReusedRetiredIdScansOutItsNewMemory) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer first;
  GbmSolidBuffer second;
  ASSERT_TRUE(first.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(second.Create(fd, kViewW, kViewH, 0xFF7A1E46u));

  const auto submit = [&](const GbmSolidBuffer& buffer, int* release) {
    IhsFrame f = FrameOf(buffer, 5);
    IhsLayer layer = LayerOf(&f, 1);
    return ihs_pv_submit_layers(producer_.view, &layer, 1, 0, release);
  };

  int first_release = -1;
  ASSERT_EQ(submit(first, &first_release), IHS_PV_OK);
  ASSERT_TRUE(Present());
  // Two planes: the view's single layer and the shell's own full-mode frame --
  // see EachLayerOfAViewGetsItsOwnPlane for why the shell's frame gets one.
  // Find the view's plane by its size; it is not necessarily first.
  const auto view_plane = [this](const std::vector<PlaneState>& ps) {
    return std::find_if(ps.begin(), ps.end(), [this](const PlaneState& p) {
      return p.crtc_w == kViewW && p.crtc_h == kViewH;
    });
  };
  std::vector<PlaneState> planes = ActivePlanes(card_.path);
  ASSERT_EQ(planes.size(), 2U);
  auto view_it = view_plane(planes);
  ASSERT_NE(view_it, planes.end()) << "the view's layer is not on a plane";
  const uint32_t first_fb = view_it->fb_id;

  ASSERT_EQ(ihs_pv_retire_buffer(producer_.view, 5), IHS_PV_OK);
  int second_release = -1;
  ASSERT_EQ(submit(second, &second_release), IHS_PV_OK);
  // One present places the new memory and displaces the old; the old memory's
  // release arrives a present or two later, off the flip that retired it.
  // Several, so it has certainly been and gone.
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(Present());
  }

  planes = ActivePlanes(card_.path);
  ASSERT_EQ(planes.size(), 2U);
  view_it = view_plane(planes);
  ASSERT_NE(view_it, planes.end()) << "the view's layer left the display";
  EXPECT_NE(view_it->fb_id, first_fb)
      << "the reused id is still scanned out through the framebuffer of the "
         "memory it used to name";
  if (second_release >= 0) {
    EXPECT_FALSE(Readable(second_release))
        << "the new frame was handed back while it is on the plane";
  }

  for (const int r : {first_release, second_release}) {
    if (r >= 0) {
      ::close(r);
    }
  }
}

// A frame reaches the producer's presented callback with the seq it was
// submitted under, timed by the flip that showed it.
TEST_F(PvHostVkmsPlanes, AFrameOnAPlaneIsReportedPresented) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));

  // The first present is the blocking modeset, which has no flip event to
  // time it; the next one flips.
  ASSERT_EQ(SubmitSeq(a, 0, 41), IHS_PV_OK);
  auto got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the first frame was never reported";
  EXPECT_EQ(got[0].seq, 41U);
  EXPECT_NE(got[0].flags & IHS_PV_PRESENTED_ZERO_COPY, 0U)
      << "a frame scanned out from the producer's buffer is zero-copy";

  ASSERT_EQ(SubmitSeq(b, 1, 42), IHS_PV_OK);
  got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the flipped frame was never reported";
  EXPECT_EQ(got[0].seq, 42U);
  constexpr uint32_t kFlipped =
      IHS_PV_PRESENTED_VSYNC | IHS_PV_PRESENTED_HW_CLOCK |
      IHS_PV_PRESENTED_HW_COMPLETION | IHS_PV_PRESENTED_ZERO_COPY;
  EXPECT_EQ(got[0].flags, kFlipped);
  EXPECT_NE(got[0].msc, 0U) << "a flip carries the CRTC's vblank count";
  EXPECT_NE(got[0].ust_ns, 0U);
  EXPECT_GT(got[0].refresh_ns, 10'000'000U) << "vkms runs at 60 Hz";
  EXPECT_LT(got[0].refresh_ns, 20'000'000U);

  // Nothing new: nothing more to report however often the view is presented.
  EXPECT_TRUE(PresentUntilReported(1, 3).empty())
      << "a frame already reported was reported again";
}

// A frame replaced before any present took it is never reported; the one that
// replaced it is.
TEST_F(PvHostVkmsPlanes, AFrameReplacedBeforeItWasShownIsNotReported) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  GbmSolidBuffer c;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
  ASSERT_TRUE(c.Create(fd, kViewW, kViewH, 0xFF461E7Au));
  ASSERT_EQ(SubmitSeq(a, 0, 1), IHS_PV_OK);
  ASSERT_EQ(PresentUntilReported(1).size(), 1U);

  ASSERT_EQ(SubmitSeq(b, 1, 2), IHS_PV_OK);
  ASSERT_EQ(SubmitSeq(c, 2, 3), IHS_PV_OK);
  const auto got = PresentUntilReported(2, 4);
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].seq, 3U) << "seq 2 was never on screen";
}

// A frame still on its way to the screen when its view is disposed is never
// reported: the plugin may already have freed what the report would reach.
TEST_F(PvHostVkmsPlanes, NoReportReachesAPluginOnceItsDisposeHasStarted) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
  ASSERT_EQ(SubmitSeq(a, 0, 1), IHS_PV_OK);
  ASSERT_EQ(PresentUntilReported(1).size(), 1U);

  // Committed, its flip not yet complete, and then the view goes away.
  ASSERT_EQ(SubmitSeq(b, 1, 2), IHS_PV_OK);
  ASSERT_TRUE(Present());
  ASSERT_TRUE(state_.platform_view_registry->Dispose(1, false));
  producer_.view = nullptr;
  ASSERT_EQ(producer_.disposed.load(), 1);

  // Let the flip complete, and a few more.
  for (int i = 0; i < 3; ++i) {
    Present();
  }
  EXPECT_EQ(producer_.late_reports.load(), 0)
      << "a presented report reached the plugin after its dispose";
}

// A layer whose buffer no plane scans out -- here, one that claims a tiling
// the vkms planes do not offer -- is hinted the formats that would reach one;
// once it switches, the hint is withdrawn, and a steady state says nothing.
TEST_F(PvHostVkmsPlanes, AnUnscannableFormatIsHintedAndThenWithdrawn) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
  struct stat st{};
  ASSERT_EQ(fstat(fd, &st), 0);

  // Layer 5 of @p buffer, claiming @p modifier.
  const auto submit = [this](const GbmSolidBuffer& buffer, uint32_t buffer_id,
                             uint64_t modifier, uint64_t seq) {
    IhsFrame f = FrameOf(buffer, buffer_id);
    f.format.modifier = modifier;
    IhsLayer layer = LayerOf(&f, 5);
    int release = -1;
    const int rc =
        ihs_pv_submit_layers(producer_.view, &layer, 1, seq, &release);
    if (release >= 0) {
      ::close(release);
    }
    return rc;
  };

  // An X-tiled claim: no vkms plane lists it.
  ASSERT_EQ(submit(a, 0, I915_FORMAT_MOD_X_TILED, 1), IHS_PV_OK);
  Present();
  auto hints = producer_.TakeHints();
  ASSERT_EQ(hints.size(), 1U) << "no hint for a format no plane scans out";
  EXPECT_EQ(hints[0].layer_id, 5U);
  EXPECT_EQ(hints[0].dev, static_cast<uint64_t>(st.st_rdev));
  ASSERT_FALSE(hints[0].formats.empty());
  const bool offers_linear_xrgb =
      std::any_of(hints[0].formats.begin(), hints[0].formats.end(),
                  [](const IhsFormatModifier& fm) {
                    return fm.fourcc == DRM_FORMAT_XRGB8888 &&
                           fm.modifier == DRM_FORMAT_MOD_LINEAR;
                  });
  EXPECT_TRUE(offers_linear_xrgb) << "the planes' own formats were not offered";

  // Switched to what the planes scan: the hint is withdrawn, once.
  ASSERT_EQ(submit(b, 1, DRM_FORMAT_MOD_LINEAR, 2), IHS_PV_OK);
  ASSERT_TRUE(Present());
  hints = producer_.TakeHints();
  ASSERT_EQ(hints.size(), 1U) << "the hint was not withdrawn";
  EXPECT_EQ(hints[0].layer_id, 5U);
  EXPECT_EQ(hints[0].formats.size(), 0U);

  // Nothing changes from here: nothing more is said.
  ASSERT_EQ(submit(a, 2, DRM_FORMAT_MOD_LINEAR, 3), IHS_PV_OK);
  ASSERT_TRUE(Present());
  EXPECT_TRUE(producer_.TakeHints().empty());
}

// The same through the GL compositor: the frame is composited, so it is not
// zero-copy, and it reaches the screen through the backend's own page flip.
class PvHostVkmsGl : public PvHostVkmsPlanes {
 protected:
  [[nodiscard]] const char* CompositorMode() const override { return "gl"; }
};

// A platform view frees its GL imports when it is disposed, on the platform
// thread, which has no GL context: glDeleteTextures there did nothing, the
// textures stayed, and some drivers kept each one's dma-buf open for good. The
// backend queues the free and runs it at the next present, on the raster
// thread with its context current.
TEST_F(PvHostVkmsGl, AFreeQueuedOffTheRasterThreadRunsAtTheNextPresent) {
  auto* drm = dynamic_cast<DrmBackend*>(view_->GetBackend());
  ASSERT_NE(drm, nullptr);
  bool ran = false;
  EGLContext context = EGL_NO_CONTEXT;
  std::thread platform([&] {
    drm->ScheduleDeferredDestroy([&] {
      ran = true;
      context = eglGetCurrentContext();
    });
  });
  platform.join();
  EXPECT_FALSE(ran) << "freed on the thread that queued it";

  ASSERT_TRUE(Present());
  EXPECT_TRUE(ran) << "the next present did not run the free";
  EXPECT_NE(context, EGL_NO_CONTEXT) << "freed with no GL context current";
}

// A frame the shell cannot import is never drawn, so nothing else releases
// it. It used to be dropped with its release still out, and a producer waited
// on that buffer for good.
TEST_F(PvHostVkmsGl, AFrameThatFailsToImportIsReleased) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  IhsFrame f = FrameOf(a, 0);
  // A tiling this device does not have: the import is refused.
  f.format.modifier = I915_FORMAT_MOD_X_TILED;
  IhsLayer layer = LayerOf(&f, 1);
  int release = -1;
  ASSERT_EQ(ihs_pv_submit_layers(producer_.view, &layer, 1, 1, &release),
            IHS_PV_OK);
  ASSERT_GE(release, 0) << "no release fence for the frame";
  ASSERT_TRUE(Present());
  EXPECT_TRUE(Readable(release)) << "a frame that failed to import was held";
  ::close(release);
}

TEST_F(PvHostVkmsGl, ACompositedFrameIsReportedPresented) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));

  ASSERT_EQ(SubmitSeq(a, 0, 7), IHS_PV_OK);
  auto got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the first frame was never reported";
  EXPECT_EQ(got[0].seq, 7U);

  ASSERT_EQ(SubmitSeq(b, 1, 8), IHS_PV_OK);
  got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the flipped frame was never reported";
  EXPECT_EQ(got[0].seq, 8U);
  EXPECT_EQ(got[0].flags, IHS_PV_PRESENTED_VSYNC | IHS_PV_PRESENTED_HW_CLOCK |
                              IHS_PV_PRESENTED_HW_COMPLETION)
      << "composited, so not zero-copy";
  EXPECT_NE(got[0].msc, 0U);
}

// Producers cycling rings get every buffer back, with more views than there
// are planes for: some views composite and some scan out, and which is which
// can change from frame to frame. Half way through, two frames arrive per
// present, so each view's first is replaced before it is shown.
TEST_F(PvHostVkmsPlanes, EveryBufferOfManyRingsComesBack) {
  constexpr uint32_t kViews = 6;
  constexpr uint32_t kRing = 3;
  ASSERT_GE(card_.mode_w, kViews * kViewW);
  const int fd = display_->SharedDevice()->fd();
  // View 1 is the fixture's; the rest are made here.
  FakeProducer extra[kViews - 1];
  std::vector<std::string> types;
  for (uint32_t v = 1; v < kViews; ++v) {
    types.push_back("views/ring-" + std::to_string(v));
  }
  for (uint32_t v = 1; v < kViews; ++v) {
    ASSERT_EQ(ihs_pv_register_factory(types[v - 1].c_str(), &fake_factory,
                                      &extra[v - 1]),
              IHS_PV_OK);
    PlatformViewRegistry::CreateRequest req{};
    req.id = static_cast<int32_t>(1 + v);
    req.view_type = types[v - 1];
    req.width = kViewW;
    req.height = kViewH;
    ASSERT_TRUE(state_.platform_view_registry->CreateViaFactory(req));
    ASSERT_NE(extra[v - 1].view, nullptr);
  }
  const auto view_of = [&](uint32_t v) {
    return v == 0 ? producer_.view : extra[v - 1].view;
  };
  const auto present_all = [&] {
    FlutterPlatformView pv[kViews]{};
    FlutterLayer layer[kViews]{};
    const FlutterLayer* layers[kViews];
    for (uint32_t v = 0; v < kViews; ++v) {
      pv[v].struct_size = sizeof(FlutterPlatformView);
      pv[v].identifier = 1 + v;
      layer[v].struct_size = sizeof(FlutterLayer);
      layer[v].type = kFlutterLayerContentTypePlatformView;
      layer[v].platform_view = &pv[v];
      layer[v].offset = FlutterPoint{static_cast<double>(v * kViewW), 0.0};
      layer[v].size =
          FlutterSize{static_cast<double>(kViewW), static_cast<double>(kViewH)};
      layers[v] = &layer[v];
    }
    return comp_->PresentLayers(layers, kViews);
  };

  GbmSolidBuffer ring[kViews][kRing];
  int out[kViews][kRing];
  for (uint32_t v = 0; v < kViews; ++v) {
    for (uint32_t i = 0; i < kRing; ++i) {
      ASSERT_TRUE(ring[v][i].Create(fd, kViewW, kViewH, 0xFF1E7A46u + v));
      out[v][i] = -1;
    }
  }
  constexpr int kFrames = 30;
  for (int frame = 0; frame < kFrames; ++frame) {
    const uint32_t id = static_cast<uint32_t>(frame) % kRing;
    for (uint32_t v = 0; v < kViews; ++v) {
      for (int i = 0; i < 10 && out[v][id] >= 0 && !Readable(out[v][id]); ++i) {
        ASSERT_TRUE(present_all());
      }
      ASSERT_TRUE(out[v][id] < 0 || Readable(out[v][id]))
          << "view " << v << " slot " << id << " was never released (frame "
          << frame << ")";
      if (out[v][id] >= 0) {
        ::close(out[v][id]);
        out[v][id] = -1;
      }
      IhsFrame f = FrameOf(ring[v][id], id);
      IhsLayer l = LayerOf(&f, 1);
      ASSERT_EQ(
          ihs_pv_submit_layers(view_of(v), &l, 1,
                               static_cast<uint64_t>(frame) + 1, &out[v][id]),
          IHS_PV_OK);
    }
    if (frame < kFrames / 2 || frame % 2 == 1) {
      ASSERT_TRUE(present_all());
    }
  }
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(present_all());
  }
  const uint32_t shown = (kFrames - 1) % kRing;
  for (uint32_t v = 0; v < kViews; ++v) {
    for (uint32_t id = 0; id < kRing; ++id) {
      if (id != shown) {
        EXPECT_TRUE(out[v][id] < 0 || Readable(out[v][id]))
            << "view " << v << " slot " << id << " is still held";
      }
      if (out[v][id] >= 0) {
        ::close(out[v][id]);
      }
    }
  }
  for (uint32_t v = 1; v < kViews; ++v) {
    state_.platform_view_registry->Dispose(static_cast<int32_t>(1 + v), false);
    ihs_pv_unregister_factory(types[v - 1].c_str());
  }
}

// The same with each producer on a thread of its own, submitting while the
// compositor presents, the way a real producer does: a submit can land while
// a present is choosing what to show, or between a commit and its flip.
TEST_F(PvHostVkmsPlanes, EveryBufferOfRingsSubmittedConcurrentlyComesBack) {
  constexpr uint32_t kViews = 4;
  constexpr uint32_t kRing = 3;
  constexpr int kFrames = 150;
  ASSERT_GE(card_.mode_w, kViews * kViewW);
  const int fd = display_->SharedDevice()->fd();
  FakeProducer extra[kViews - 1];
  std::vector<std::string> types;
  for (uint32_t v = 1; v < kViews; ++v) {
    types.push_back("views/concurrent-" + std::to_string(v));
  }
  for (uint32_t v = 1; v < kViews; ++v) {
    ASSERT_EQ(ihs_pv_register_factory(types[v - 1].c_str(), &fake_factory,
                                      &extra[v - 1]),
              IHS_PV_OK);
    PlatformViewRegistry::CreateRequest req{};
    req.id = static_cast<int32_t>(1 + v);
    req.view_type = types[v - 1];
    req.width = kViewW;
    req.height = kViewH;
    ASSERT_TRUE(state_.platform_view_registry->CreateViaFactory(req));
    ASSERT_NE(extra[v - 1].view, nullptr);
  }
  const auto view_of = [&](uint32_t v) {
    return v == 0 ? producer_.view : extra[v - 1].view;
  };
  GbmSolidBuffer ring[kViews][kRing];
  for (uint32_t v = 0; v < kViews; ++v) {
    for (uint32_t i = 0; i < kRing; ++i) {
      ASSERT_TRUE(ring[v][i].Create(fd, kViewW, kViewH, 0xFF1E7A46u + v));
    }
  }

  std::atomic<uint32_t> done{0};
  // Per view: the frame and slot it gave up waiting on, or -1.
  std::atomic<int> stalled_frame[kViews];
  std::atomic<int> stalled_slot[kViews];
  std::vector<std::thread> producers;
  for (uint32_t v = 0; v < kViews; ++v) {
    stalled_frame[v] = -1;
    stalled_slot[v] = -1;
    producers.emplace_back([&, v] {
      int out[kRing] = {-1, -1, -1};
      for (int frame = 0; frame < kFrames; ++frame) {
        const uint32_t id = static_cast<uint32_t>(frame) % kRing;
        if (out[id] >= 0) {
          pollfd pfd{out[id], POLLIN, 0};
          if (::poll(&pfd, 1, 2000) != 1) {
            stalled_frame[v] = frame;
            stalled_slot[v] = static_cast<int>(id);
            break;
          }
          ::close(out[id]);
          out[id] = -1;
        }
        IhsFrame f = FrameOf(ring[v][id], id);
        IhsLayer l = LayerOf(&f, 1);
        if (ihs_pv_submit_layers(view_of(v), &l, 1,
                                 static_cast<uint64_t>(frame) + 1,
                                 &out[id]) != IHS_PV_OK) {
          break;
        }
        // Faster than the display some of the time, so frames are replaced
        // before they are shown; slower the rest.
        std::this_thread::sleep_for(
            std::chrono::milliseconds((frame / 10) % 2 == 0 ? 4 : 20));
      }
      for (int r : out) {
        if (r >= 0) {
          ::close(r);
        }
      }
      done.fetch_add(1);
    });
  }

  FlutterPlatformView pv[kViews]{};
  FlutterLayer layer[kViews]{};
  const FlutterLayer* layers[kViews];
  for (uint32_t v = 0; v < kViews; ++v) {
    pv[v].struct_size = sizeof(FlutterPlatformView);
    pv[v].identifier = 1 + v;
    layer[v].struct_size = sizeof(FlutterLayer);
    layer[v].type = kFlutterLayerContentTypePlatformView;
    layer[v].platform_view = &pv[v];
    layer[v].offset = FlutterPoint{static_cast<double>(v * kViewW), 0.0};
    layer[v].size =
        FlutterSize{static_cast<double>(kViewW), static_cast<double>(kViewH)};
    layers[v] = &layer[v];
  }
  for (int i = 0; i < 4000 && done.load() < kViews; ++i) {
    comp_->PresentLayers(layers, kViews);
  }
  for (auto& t : producers) {
    t.join();
  }
  for (uint32_t v = 0; v < kViews; ++v) {
    EXPECT_EQ(stalled_frame[v].load(), -1)
        << "view " << v << " waited for good on slot " << stalled_slot[v]
        << " at frame " << stalled_frame[v];
  }
  for (uint32_t v = 1; v < kViews; ++v) {
    state_.platform_view_registry->Dispose(static_cast<int32_t>(1 + v), false);
    ihs_pv_unregister_factory(types[v - 1].c_str());
  }
}

// The same for a ring of two: the first buffer comes back once the second is
// on screen, with no present after that. The scene used to hold a displaced
// buffer until its next commit, so a producer with two buffers that waits for
// one before rendering waited for good.
TEST_F(PvHostVkmsPlanes, ARingOfTwoGetsItsFirstBackWithoutAnotherPresent) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer ring[2];
  int release[2] = {-1, -1};
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(ring[i].Create(fd, kViewW, kViewH, 0xFF1E7A46u + i));
  }
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(SubmitKeepingRelease(ring[i], i, i + 1, &release[i]), IHS_PV_OK);
    ASSERT_GE(release[i], 0) << "no release fence for slot " << i;
    ASSERT_TRUE(Present());
  }
  // Nothing presents from here on.
  pollfd pfd{release[0], POLLIN, 0};
  EXPECT_EQ(::poll(&pfd, 1, 500), 1)
      << "slot 0 waits for a commit that a producer out of buffers never "
         "asks for";
  EXPECT_FALSE(Readable(release[1])) << "the buffer on screen was released";
  for (int r : release) {
    ::close(r);
  }
}

// A producer with a ring of three gets its first buffer back once its third
// is on screen, with no present after that. Each submit asks for one present
// and no more, so a producer out of free buffers asks for none: a release
// that waits for another present never comes, and the producer waits for
// good. The releases used to wait for the top of the next present.
TEST_F(PvHostVkmsPlanes, ARingOfThreeGetsItsFirstBackWithoutAnotherPresent) {
  const int fd = display_->SharedDevice()->fd();
  constexpr int kRing = 3;
  GbmSolidBuffer ring[kRing];
  int release[kRing] = {-1, -1, -1};
  for (int i = 0; i < kRing; ++i) {
    ASSERT_TRUE(ring[i].Create(fd, kViewW, kViewH, 0xFF1E7A46u + i));
  }
  for (int i = 0; i < kRing; ++i) {
    ASSERT_EQ(SubmitKeepingRelease(ring[i], i, i + 1, &release[i]), IHS_PV_OK);
    ASSERT_GE(release[i], 0) << "no release fence for slot " << i;
    ASSERT_TRUE(Present());
  }
  // Nothing presents from here on.
  pollfd pfd{release[0], POLLIN, 0};
  EXPECT_EQ(::poll(&pfd, 1, 500), 1)
      << "slot 0 waits for a present that a producer out of buffers never "
         "asks for";
  EXPECT_FALSE(Readable(release[kRing - 1]))
      << "the buffer on screen was released";
  for (int r : release) {
    ::close(r);
  }
}

// A frame of platform views alone has the shell's background under it, on a
// plane covering the mode; one with a backing store at the bottom has that
// store there instead, and going back brings the background back.
TEST_F(PvHostVkmsPlanes, AFrameWithNoBackingStoreHasTheBackgroundUnderIt) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  const auto planes_showing = [this] {
    const std::vector<PlaneState> planes = ActivePlanes(card_.path);
    int full = 0;
    int view = 0;
    for (const PlaneState& p : planes) {
      full += p.crtc_w == card_.mode_w && p.crtc_h == card_.mode_h ? 1 : 0;
      view += p.crtc_w == kViewW && p.crtc_h == kViewH ? 1 : 0;
    }
    return std::make_tuple(planes.size(), full, view);
  };

  ASSERT_EQ(SubmitSeq(a, 0, 1), IHS_PV_OK);
  ASSERT_TRUE(Present());
  auto [count, full, view] = planes_showing();
  EXPECT_EQ(full, 1) << "no plane covers the mode under the view";
  EXPECT_EQ(view, 1);
  EXPECT_EQ(count, 2U);

  FlutterBackingStoreConfig cfg{};
  cfg.struct_size = sizeof(FlutterBackingStoreConfig);
  cfg.size = FlutterSize{static_cast<double>(card_.mode_w),
                         static_cast<double>(card_.mode_h)};
  FlutterBackingStore bs{};
  ASSERT_TRUE(comp_->CreateBackingStore(&cfg, &bs));
  FlutterLayer ui{};
  ui.struct_size = sizeof(FlutterLayer);
  ui.type = kFlutterLayerContentTypeBackingStore;
  ui.backing_store = &bs;
  ui.size = cfg.size;
  FlutterPlatformView pv{};
  pv.struct_size = sizeof(FlutterPlatformView);
  pv.identifier = 1;
  FlutterLayer layer{};
  layer.struct_size = sizeof(FlutterLayer);
  layer.type = kFlutterLayerContentTypePlatformView;
  layer.platform_view = &pv;
  layer.size =
      FlutterSize{static_cast<double>(kViewW), static_cast<double>(kViewH)};
  const FlutterLayer* with_store[] = {&ui, &layer};
  ASSERT_TRUE(comp_->PresentLayers(with_store, 2));
  std::tie(count, full, view) = planes_showing();
  EXPECT_EQ(full, 1) << "the backing store is not on a plane";
  EXPECT_EQ(count, 2U) << "the background stayed beside the backing store";

  ASSERT_TRUE(Present());
  std::tie(count, full, view) = planes_showing();
  EXPECT_EQ(full, 1) << "the background did not come back";
  EXPECT_EQ(view, 1);
  EXPECT_EQ(count, 2U);
  comp_->CollectBackingStore(&bs);
}

TEST_F(PvHostVkmsPlanes, AFrameOnAPlaneIsReleasedWhenTheViewLeavesFirst) {
  ExpectAFrameOnAPlaneReleasedOnceTheViewLeaves(/*dispose_first=*/false);
}

TEST_F(PvHostVkmsPlanes, AFrameOnAPlaneIsReleasedWhenTheViewIsDisposedFirst) {
  ExpectAFrameOnAPlaneReleasedOnceTheViewLeaves(/*dispose_first=*/true);
}

TEST_F(PvHostVkmsGl, AFrameHeldAtDisposeIsStillReleased) {
  ExpectAHeldFrameReleasedAfterDispose();
}

// A view disposed while a release is still queued for it, and a new view in
// its place: the release must not reach the new view. It used to run against
// the disposed view's freed memory, which the new view most often reuses, and
// retire the new view's frame before it was shown. That view never reached the
// screen, and its producer ran out of buffers.
TEST_F(PvHostVkmsGl, AReleaseQueuedForADisposedViewMissesItsSuccessor) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  GbmSolidBuffer c;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));
  ASSERT_TRUE(c.Create(fd, kViewW, kViewH, 0xFF461E7Au));
  ASSERT_EQ(SubmitSeq(a, 0, 1), IHS_PV_OK);
  ASSERT_EQ(PresentUntilReported(1).size(), 1U);

  // b displaces a, whose release waits on this flip; the view goes first.
  ASSERT_EQ(SubmitSeq(b, 1, 2), IHS_PV_OK);
  ASSERT_TRUE(Present());
  ASSERT_TRUE(state_.platform_view_registry->Dispose(1, false));
  producer_.view = nullptr;
  producer_.TakePresented();

  PlatformViewRegistry::CreateRequest req{};
  req.id = 1;
  req.view_type = kViewType;
  req.width = kViewW;
  req.height = kViewH;
  ASSERT_TRUE(state_.platform_view_registry->CreateViaFactory(req));
  ASSERT_NE(producer_.view, nullptr);

  // The new view's first frame, under the buffer id a's release names.
  ASSERT_EQ(SubmitSeq(c, 0, 3), IHS_PV_OK);
  const auto got = PresentUntilReported(1);
  ASSERT_EQ(got.size(), 1U) << "the new view's first frame was never shown";
  EXPECT_EQ(got[0].seq, 3U);
}

// An early flip completion must not leave the latch armed.
//
// The legacy present path publishes its flip state and raises flip_pending_
// around a NONBLOCK ioctl. If the card's reader dispatches that flip's
// completion before the raster thread gets back, a latch raised afterwards
// describes a flip that has already retired -- and the next vsync baton parks
// against it with no event coming to return it. The display stops (#649).
//
// on_commit_returned_ forces exactly that order: it runs on the raster thread
// with the flip in flight, and blocks until the reader has handled it. What is
// asserted afterwards is the latch, because that is the thing that goes stale.
// On a build that raises the latch after the ioctl this fails.
TEST_F(DrmBackendVkms, AnEarlyFlipCompletionLeavesNoStaleLatch) {
  auto view = std::make_shared<FakePlatformView>(11);
  view->set_texture(tex_);
  compositor_->RegisterSurface(11, view);

  // First present brings the pipeline up; the flip that matters is the next.
  ASSERT_TRUE(PresentPlatformView(11));

  const uint64_t before = backend_->FlipsHandledForTest();
  backend_->on_commit_returned_ = [this, before] {
    // Bounded: if the reader never dispatches, say so rather than hang the
    // suite. Two seconds is ~120 vblanks at 60Hz.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (backend_->FlipsHandledForTest() == before &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  };
  const bool presented = PresentPlatformView(11);
  backend_->on_commit_returned_ = nullptr;
  ASSERT_TRUE(presented);

  ASSERT_GT(backend_->FlipsHandledForTest(), before)
      << "the completion never landed inside the commit window, so this case "
         "did not reproduce the order it exists to test";

  EXPECT_FALSE(backend_->FlipPendingForTest())
      << "the flip retired while we were still in Present, so nothing is in "
         "flight -- a latch left up here parks the next baton forever (#649)";

  compositor_->UnregisterSurface(11);
}

// An early flip completion must not strand a slot generation.
//
// The backing-store slot pipeline is what decides when Flutter may render into
// a pool slot again. PresentLayers used to push the generation it had just
// committed onto in_flight_slots_ *after* the commit; if the card's reader
// dispatched that flip first, the handler popped nothing and the generation
// arrived a beat late. Every later event then freed one generation too old, so
// at pool_size 3 no slot was ever Free when the next frame needed one and the
// rotate fell back to reusing the buffer being scanned out.
//
// on_commit_returned_ forces that order: it runs on the raster thread with the
// commit in flight and blocks until the reader has handled it. The assertion is
// the gate-miss counter, which is exactly "the rotate found no free slot".
TEST_F(DrmBackendVkmsScene, AnEarlyFlipCompletionDoesNotStrandASlotGeneration) {
  // A Flutter backing-store layer, not a platform view: only a backing store
  // owns a rotating slot pool, so a platform-view-only frame never reaches the
  // code under test.
  FlutterBackingStoreConfig cfg{};
  cfg.struct_size = sizeof(FlutterBackingStoreConfig);
  cfg.size = FlutterSize{1024.0, 768.0};
  FlutterBackingStore bs{};
  ASSERT_TRUE(compositor_->CreateBackingStore(&cfg, &bs));

  FlutterLayer layer{};
  layer.struct_size = sizeof(FlutterLayer);
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &bs;
  layer.offset = FlutterPoint{0.0, 0.0};
  layer.size = cfg.size;
  const FlutterLayer* layers[] = {&layer};

  // Bring the pipeline up: the first commit is a blocking modeset and gets no
  // event, so the generation that matters is a later one.
  ASSERT_TRUE(compositor_->PresentLayers(layers, 1));
  ASSERT_TRUE(compositor_->PresentLayers(layers, 1));
  ASSERT_GT(compositor_->SlotRotationsForTest(), 0U)
      << "no slot was rotated, so this case never reached the pipeline it "
         "exists to test -- a backing store with pool_size > 1 is required";

  // Force the completion to land inside the commit window, once.
  bool forced = false;
  compositor_->on_commit_returned_ = [this, &forced] {
    if (forced) {
      return;
    }
    forced = true;
    const uint64_t before = backend_->FlipsHandledForTest();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (backend_->FlipsHandledForTest() == before &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  };
  ASSERT_TRUE(compositor_->PresentLayers(layers, 1));
  compositor_->on_commit_returned_ = nullptr;
  ASSERT_TRUE(forced) << "the hook never ran, so the order was never forced";

  // Now run the pool round. With the generation published before the commit
  // the pipeline stays in step and a slot is always free; published after, it
  // is one generation behind from here on and the gate starves.
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(compositor_->PresentLayers(layers, 1)) << "frame " << i;
  }

  EXPECT_EQ(compositor_->SlotGateMissesForTest(), 0U)
      << "the rotate found no free slot, so it reused a buffer the kernel is "
         "still scanning out -- the generation reached the pipeline late";
}

// An early flip completion must report the frame it showed, not the one before.
//
// OnFlipEvent reads atomic_serial_ to name the serial a flip carried. That
// serial used to be published *after* the commit, so when the card's reader
// dispatched the flip before the raster thread returned from the ioctl, the
// event named serial-1 -- a frame already reported -- and the frame actually on
// screen went unreported. The skew is permanent: each commit adds one serial
// and each event consumes one, so the offset never closes and every later frame
// is reported one flip late.
//
// on_commit_returned_ forces that order once. What is asserted is the
// producer's own feedback, which is the thing a late report actually breaks.
TEST_F(PvHostVkmsPlanes, AnEarlyFlipCompletionReportsTheFrameItShowed) {
  const int fd = display_->SharedDevice()->fd();
  GbmSolidBuffer a;
  GbmSolidBuffer b;
  ASSERT_TRUE(a.Create(fd, kViewW, kViewH, 0xFF1E7A46u));
  ASSERT_TRUE(b.Create(fd, kViewW, kViewH, 0xFF7A1E46u));

  // Bring the pipeline up. The first present is the blocking modeset, reported
  // inline, so the commit that matters is the next one.
  ASSERT_EQ(SubmitSeq(a, 0, 11), IHS_PV_OK);
  ASSERT_EQ(PresentUntilReported(1).size(), 1U)
      << "the first frame was never reported, so the case cannot run";

  // Block on the raster thread with the flip in flight, until the reader has
  // dispatched its completion.
  auto* drm = dynamic_cast<DrmBackend*>(view_->GetBackend());
  ASSERT_NE(drm, nullptr);
  bool forced = false;
  comp_->on_commit_returned_ = [drm, &forced] {
    if (forced) {
      return;
    }
    forced = true;
    const uint64_t before = drm->FlipsHandledForTest();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (drm->FlipsHandledForTest() == before &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  };
  ASSERT_EQ(SubmitSeq(b, 1, 12), IHS_PV_OK);
  // Exactly one present: a second would report this frame off the *next* flip
  // and hide the defect.
  const auto got = PresentUntilReported(1, 1);
  comp_->on_commit_returned_ = nullptr;
  ASSERT_TRUE(forced) << "the hook never ran, so the order was never forced";

  ASSERT_EQ(got.size(), 1U)
      << "the flip that showed this frame reported nothing -- it named the "
         "previous serial, which was already finished";
  EXPECT_EQ(got[0].seq, 12U)
      << "reported a frame other than the one this flip showed";
}

// A flip event that never arrives must not leave Flutter waiting forever.
//
// The baton comes back on the flip event for the commit in flight. If that
// event is lost the baton is never returned, no frame is built, and because no
// frame is built Present never runs -- so its own WaitForPendingFlip guard
// never fires either. The display stops until a VT switch (#649). The detector
// watches from the task runner's io_context, which is alive exactly then.
//
// Two ticks with no progress is the signal, so one tick must not fire: a baton
// parked for less than the threshold is just a frame in flight.
TEST_F(DrmBackendVkms, AStalledFlipIsRecoveredNotWaitedOnForever) {
  auto view = std::make_shared<FakePlatformView>(31);
  view->set_texture(tex_);
  compositor_->RegisterSurface(31, view);
  ASSERT_TRUE(PresentPlatformView(31));

  // Delivery needs a runner: PostOnVsync marshals onto it, and without one the
  // provider leaves the baton parked whatever the detector decides. Wire it on
  // the provider directly rather than through SetPlatformTaskRunner, which
  // would also start the real timer on an io_context nothing here runs.
  FLUTTER_API_SYMBOL(FlutterEngine) no_engine{nullptr};
  TaskRunner runner("stall-test", no_engine);
  backend_->VsyncForTest().SetEngine(nullptr, &runner);

  // The state the detector exists for: a commit in flight whose event will
  // never arrive, and Flutter waiting on it.
  backend_->SetFlipPendingForTest(true);
  backend_->VsyncForTest().SubmitBaton(nullptr, 0x5151);
  ASSERT_TRUE(backend_->VsyncForTest().HasParkedBaton())
      << "the latch is up, so the baton must park -- otherwise there is no "
         "stall to detect and this case proves nothing";

  const uint64_t before = backend_->StallRecoveriesForTest();
  backend_->CheckForStallForTest();
  EXPECT_TRUE(backend_->VsyncForTest().HasParkedBaton())
      << "one tick is not evidence of a stall; a frame may simply be in flight";
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before);

  // Second tick, nothing moved in between.
  backend_->CheckForStallForTest();
  EXPECT_FALSE(backend_->FlipPendingForTest())
      << "the latch has to go, or the baton parks again immediately";
  EXPECT_FALSE(backend_->VsyncForTest().HasParkedBaton())
      << "the baton must be handed back; nothing else is going to";
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before + 1);

  compositor_->UnregisterSurface(31);
}

// The other half: a display that is merely busy must never be recovered. Flips
// advancing between ticks is progress, however long a baton has been parked.
TEST_F(DrmBackendVkms, AFlipInFlightIsNotMistakenForAStall) {
  auto view = std::make_shared<FakePlatformView>(32);
  view->set_texture(tex_);
  compositor_->RegisterSurface(32, view);
  ASSERT_TRUE(PresentPlatformView(32));

  const uint64_t before = backend_->StallRecoveriesForTest();
  for (int i = 0; i < 4; ++i) {
    // A tick between real frames. Presenting advances flips_handled_, which is
    // the progress the detector looks for -- so however a baton happens to be
    // parked at the moment of the tick, nothing here is stalled.
    backend_->CheckForStallForTest();
    ASSERT_TRUE(PresentPlatformView(32)) << "frame " << i;
  }
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before)
      << "flips were advancing the whole time; nothing was stalled";

  compositor_->UnregisterSurface(32);
}

// A revoked session looks exactly like a lost flip event -- a baton parked, the
// latch up, no events coming -- and must not be reported as one. The latch
// stays up across a VT switch-out by design; OnResume is what clears it.
TEST_F(DrmBackendVkms, APausedSessionIsNotReportedAsAStall) {
  auto view = std::make_shared<FakePlatformView>(33);
  view->set_texture(tex_);
  compositor_->RegisterSurface(33, view);
  ASSERT_TRUE(PresentPlatformView(33));

  FLUTTER_API_SYMBOL(FlutterEngine) no_engine{nullptr};
  TaskRunner runner("stall-pause-test", no_engine);
  backend_->VsyncForTest().SetEngine(nullptr, &runner);

  const uint64_t before = backend_->StallRecoveriesForTest();
  backend_->OnSessionPaused();
  // Put the latch back: OnSessionPaused drops the legacy one, but the scene
  // path's stays up until OnResume -- which is the case this guards.
  backend_->SetFlipPendingForTest(true);
  backend_->VsyncForTest().SubmitBaton(nullptr, 0x5353);
  ASSERT_TRUE(backend_->VsyncForTest().HasParkedBaton())
      << "no baton parked, so there is no stall to mistake this for";

  for (int i = 0; i < 4; ++i) {
    backend_->CheckForStallForTest();
  }
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before)
      << "a VT switch-out was reported as a lost flip event";
  EXPECT_TRUE(backend_->FlipPendingForTest())
      << "the latch is OnResume's to clear while the session is revoked";

  backend_->OnSessionResumed(display_->SharedDevice()->fd());
  compositor_->UnregisterSurface(33);
}

// #732: the refresh rate the engine is told must come from the selected mode,
// not the 60.0 the display is constructed with.
//
// This needs a mode whose vrefresh is not 60, or it passes against the old
// hardcoded value and proves nothing. A default vkms offers 800x600@56
// alongside its 60Hz modes, which is what this pins; it skips rather than
// lies if the card on this host has no such mode.
class DrmBackendVkmsRefreshRate : public ::testing::Test {
 protected:
  void SetUp() override {
    card_ = FindVkms();
    if (card_.path.empty()) {
      GTEST_SKIP() << "no vkms card on this host (sudo modprobe vkms)";
    }
    if (!card_.ok()) {
      GTEST_SKIP() << card_.path << " is vkms but exposes no connected "
                   << "connector with modes";
    }
    display_ = std::make_unique<DrmDisplay>(0, 0, 60.0, card_.path,
                                            /*no_seat=*/true);
    if (display_->SharedDevice() == nullptr) {
      GTEST_SKIP() << "no DRM master on " << card_.path;
    }
  }

  std::unique_ptr<DrmBackend> BackendWithMode(const std::string& spec) {
    DrmConfig cfg{card_.path, std::nullopt, std::nullopt,
                  /*debug_backend=*/false};
    cfg.no_seat = true;
    cfg.disable_cursor = true;
    cfg.mode_spec = spec;
    return DrmBackend::Create(cfg, display_->session(),
                              display_->SharedDevice(), display_.get());
  }

  VkmsCard card_;
  std::unique_ptr<DrmDisplay> display_;
};

TEST_F(DrmBackendVkmsRefreshRate, ReportsTheSelectedModesRateNot60) {
  auto backend = BackendWithMode("800x600@56");
  if (backend == nullptr) {
    GTEST_SKIP() << "this vkms has no 800x600@56 mode; nothing non-60 to pin";
  }

  // The selected mode really is the one asked for, so the rate below is its.
  EXPECT_EQ(backend->vrefresh(), 56u);

  // What FlutterView reports as FlutterEngineDisplay::refresh_rate.
  EXPECT_NEAR(backend->RefreshRateHz(), 56.0, 0.5);
  EXPECT_GT(backend->RefreshRateHz(), 0.0);
  EXPECT_LT(backend->RefreshRateHz(), 59.0) << "the hardcoded 60 is back";

  // Derived from the period, so the two cannot disagree.
  EXPECT_GT(backend->RefreshPeriodNs(), 0u);
  EXPECT_NEAR(backend->RefreshRateHz(),
              1e9 / static_cast<double>(backend->RefreshPeriodNs()), 1e-6);
}

TEST_F(DrmBackendVkmsRefreshRate, A60HzModeStillReports60) {
  auto backend = BackendWithMode("1024x768@60");
  if (backend == nullptr) {
    GTEST_SKIP() << "this vkms has no 1024x768@60 mode";
  }
  EXPECT_EQ(backend->vrefresh(), 60u);
  EXPECT_NEAR(backend->RefreshRateHz(), 60.0, 0.5);
}

// #765: a connector reconfigure re-resolves the mode. A refresh-only change is
// adopted on the spot; an extent change is latched for the raster thread,
// which rebuilds the scanout surfaces at the new extent.
//
// vkms exposes connector status through configfs but no way to change a
// connector's mode, so a genuine same-connector reconfigure cannot be
// synthesized here. The resolve is re-run directly instead, and the extent
// cases go through RequestModeChange with a mode taken from the connector's
// own list -- so the mode asked for is one this card would really drive.
class DrmBackendVkmsReconfigure : public ::testing::Test {
 protected:
  void SetUp() override {
    card_ = FindVkms();
    if (card_.path.empty()) {
      GTEST_SKIP() << "no vkms card on this host (sudo modprobe vkms)";
    }
    if (!card_.ok()) {
      GTEST_SKIP() << card_.path << " is vkms but exposes no connected "
                   << "connector with modes";
    }
    display_ = std::make_unique<DrmDisplay>(0, 0, 60.0, card_.path,
                                            /*no_seat=*/true);
    if (display_->SharedDevice() == nullptr) {
      GTEST_SKIP() << "no DRM master on " << card_.path;
    }
  }

  std::unique_ptr<DrmBackend> BackendWithMode(const std::string& spec) {
    DrmConfig cfg{card_.path, std::nullopt, std::nullopt,
                  /*debug_backend=*/false};
    cfg.no_seat = true;
    cfg.disable_cursor = true;
    cfg.mode_spec = spec;
    return DrmBackend::Create(cfg, display_->session(),
                              display_->SharedDevice(), display_.get());
  }

  // A mode this card offers whose extent differs from @p current. Taken from
  // the connector rather than synthesized, so the rebuild under test is asked
  // for something the card can actually be driven at. nullopt when the card
  // offers only one extent, which is a skip rather than a failure.
  std::optional<drmModeModeInfo> AModeOfADifferentExtent(
      const drmModeModeInfo& current) const {
    const int fd = display_->SharedDevice()->fd();
    drmModeRes* res = drmModeGetResources(fd);
    if (res == nullptr) {
      return std::nullopt;
    }
    std::optional<drmModeModeInfo> found;
    for (int i = 0; i < res->count_connectors && !found.has_value(); ++i) {
      drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
      if (c == nullptr) {
        continue;
      }
      if (c->connection == DRM_MODE_CONNECTED) {
        for (int m = 0; m < c->count_modes; ++m) {
          if (c->modes[m].hdisplay != current.hdisplay ||
              c->modes[m].vdisplay != current.vdisplay) {
            found = c->modes[m];
            break;
          }
        }
      }
      drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return found;
  }

  VkmsCard card_;
  std::unique_ptr<DrmDisplay> display_;
};

// Nothing changed: the connector still reports the mode we picked, so the
// re-resolve is a no-op and the caller is told not to bother re-reporting.
TEST_F(DrmBackendVkmsReconfigure, AnUnchangedConnectorAdoptsNothing) {
  auto backend = BackendWithMode("1024x768@60");
  if (backend == nullptr) {
    GTEST_SKIP() << "this vkms has no 1024x768@60 mode";
  }
  const uint32_t before = backend->RefreshPeriodNs();

  EXPECT_FALSE(backend->OnOutputReconfigured());
  EXPECT_EQ(backend->RefreshPeriodNs(), before);
  EXPECT_EQ(backend->vrefresh(), 60u);
}

// The decision itself, over synthetic modes, because the vkms path cannot
// reach two of the three branches: with a mode_spec set, re-resolving picks
// the same mode and stops at kNone before any refusal is considered. An
// earlier version of this file asserted the refusal and passed for that
// reason, proving nothing.
//
// 1920x1080 at 60 and at 59.94 is the case that matters: both report
// vrefresh 60, so a classifier comparing on vrefresh would call them
// identical and never adopt the change (#762).
TEST(DrmBackendModeChange, SameTimingIsNoChange) {
  drmModeModeInfo a{};
  a.hdisplay = 1920;
  a.vdisplay = 1080;
  a.clock = 148500;
  a.htotal = 2200;
  a.vtotal = 1125;
  a.vrefresh = 60;
  EXPECT_EQ(DrmBackend::ClassifyModeChange(a, a),
            DrmBackend::ModeChange::kNone);
}

TEST(DrmBackendModeChange, A5994VariantIsARefreshOnlyChange) {
  drmModeModeInfo sixty{};
  sixty.hdisplay = 1920;
  sixty.vdisplay = 1080;
  sixty.clock = 148500;
  sixty.htotal = 2200;
  sixty.vtotal = 1125;
  sixty.vrefresh = 60;

  drmModeModeInfo fifty_nine_94 = sixty;
  fifty_nine_94.clock = 148352;  // same totals, different pixel clock
  ASSERT_EQ(fifty_nine_94.vrefresh, sixty.vrefresh)
      << "the point of this case is that vrefresh cannot tell them apart";

  EXPECT_EQ(DrmBackend::ClassifyModeChange(fifty_nine_94, sixty),
            DrmBackend::ModeChange::kRefreshOnly);
}

TEST(DrmBackendModeChange, ADifferentExtentIsAnExtentChange) {
  drmModeModeInfo current{};
  current.hdisplay = 1920;
  current.vdisplay = 1080;
  current.clock = 148500;
  current.htotal = 2200;
  current.vtotal = 1125;
  current.vrefresh = 60;

  drmModeModeInfo smaller = current;
  smaller.hdisplay = 1280;
  smaller.vdisplay = 720;
  EXPECT_EQ(DrmBackend::ClassifyModeChange(smaller, current),
            DrmBackend::ModeChange::kExtent);

  // kExtent even when the timing also differs, so the extent check has to come
  // first: the surface rebuild subsumes the re-modeset, and classifying this
  // as kRefreshOnly would adopt the new timing at the old extent.
  smaller.clock = 74250;
  EXPECT_EQ(DrmBackend::ClassifyModeChange(smaller, current),
            DrmBackend::ModeChange::kExtent);

  // Each axis alone is enough. A mode that only grows vertically is the
  // rotation case, and an hdisplay-only comparison would miss it.
  drmModeModeInfo taller = current;
  taller.vdisplay = 1200;
  EXPECT_EQ(DrmBackend::ClassifyModeChange(taller, current),
            DrmBackend::ModeChange::kExtent);
  drmModeModeInfo wider = current;
  wider.hdisplay = 2560;
  EXPECT_EQ(DrmBackend::ClassifyModeChange(wider, current),
            DrmBackend::ModeChange::kExtent);
}

// #765: the FB extent rule, which an extent change re-runs. Pure, so both the
// configured and the clamped case are reachable without a card -- and the
// clamp is the one a change can newly trigger: a request that fitted the old
// mode need not fit a smaller new one.
TEST(DrmBackendFbExtent, FullScreenWhenNothingIsConfigured) {
  drmModeModeInfo mode{};
  mode.hdisplay = 1920;
  mode.vdisplay = 1080;
  const auto [w, h] =
      DrmBackend::FbExtentForMode(std::nullopt, std::nullopt, mode);
  EXPECT_EQ(w, 1920u);
  EXPECT_EQ(h, 1080u);
}

TEST(DrmBackendFbExtent, AConfiguredSizeWinsAndSurvivesAModeChange) {
  drmModeModeInfo mode{};
  mode.hdisplay = 1920;
  mode.vdisplay = 1080;
  auto [w, h] = DrmBackend::FbExtentForMode(1280u, 720u, mode);
  EXPECT_EQ(w, 1280u);
  EXPECT_EQ(h, 720u);

  // Still framed at the same size on a bigger mode: the request is the FB,
  // not a fraction of the panel.
  mode.hdisplay = 2560;
  mode.vdisplay = 1440;
  std::tie(w, h) = DrmBackend::FbExtentForMode(1280u, 720u, mode);
  EXPECT_EQ(w, 1280u);
  EXPECT_EQ(h, 720u);
}

TEST(DrmBackendFbExtent, AModeSmallerThanTheRequestClampsPerAxis) {
  drmModeModeInfo mode{};
  mode.hdisplay = 1024;
  mode.vdisplay = 768;

  // CRTC_W greater than the mode fails the atomic TEST, so the clamp is what
  // keeps a configured size valid across a shrink.
  auto [w, h] = DrmBackend::FbExtentForMode(1920u, 1080u, mode);
  EXPECT_EQ(w, 1024u);
  EXPECT_EQ(h, 768u);

  // Per axis, not all-or-nothing: a request that overflows only one axis keeps
  // its own value on the other.
  std::tie(w, h) = DrmBackend::FbExtentForMode(1920u, 480u, mode);
  EXPECT_EQ(w, 1024u);
  EXPECT_EQ(h, 480u);
}

// #765, the other half: an extent change applied for real. The classifier
// tests above cover the decision; this covers the rebuild, which is where the
// EGL/GBM lifetime mistakes would be. vkms offers more than one extent, so the
// mode requested here is one the card would actually drive.
TEST_F(DrmBackendVkmsReconfigure, AnExtentChangeIsLatchedThenApplied) {
  auto backend = BackendWithMode("1024x768@60");
  if (backend == nullptr) {
    GTEST_SKIP() << "this vkms has no 1024x768@60 mode";
  }
  const auto other = AModeOfADifferentExtent(backend->mode());
  if (!other.has_value()) {
    GTEST_SKIP() << "this vkms offers only one extent; nothing to change to";
  }

  // Requested, not applied. Nothing the raster thread owns has moved yet --
  // the point of the split, since the surfaces are still being drawn into.
  EXPECT_EQ(backend->RequestModeChange(*other),
            DrmBackend::ModeChange::kExtent);
  EXPECT_EQ(backend->mode_width(), 1024u);
  EXPECT_EQ(backend->mode_height(), 768u);
  EXPECT_EQ(backend->width(), 1024u);
  EXPECT_EQ(backend->height(), 768u);

  // The raster-thread half, with the context the renderer's present hook would
  // have made current.
  ASSERT_TRUE(backend->MakeCurrent());
  ASSERT_TRUE(backend->ApplyPendingExtentChange());

  EXPECT_EQ(backend->mode_width(), other->hdisplay);
  EXPECT_EQ(backend->mode_height(), other->vdisplay);
  // No configured size, so the FB follows the mode.
  EXPECT_EQ(backend->width(), other->hdisplay);
  EXPECT_EQ(backend->height(), other->vdisplay);
  // Derived from the new timings, so what the engine paces to and what is on
  // screen cannot disagree (#758).
  EXPECT_EQ(backend->RefreshPeriodNs(),
            ModeRefreshPeriodNs(other->clock, other->htotal, other->vtotal));

  // The latch is consumed. A second pass must not tear down surfaces that are
  // live again.
  EXPECT_FALSE(backend->ApplyPendingExtentChange());

  // The surfaces really did come back: a present has something to swap, and
  // the modeset the rebuild armed goes out on it.
  EXPECT_TRUE(backend->Present());
}

// Re-requesting the extent we just adopted is kNone, so a uevent storm does
// not rebuild the ring once per event.
TEST_F(DrmBackendVkmsReconfigure, ReRequestingTheAppliedModeIsNoChange) {
  auto backend = BackendWithMode("1024x768@60");
  if (backend == nullptr) {
    GTEST_SKIP() << "this vkms has no 1024x768@60 mode";
  }
  const auto other = AModeOfADifferentExtent(backend->mode());
  if (!other.has_value()) {
    GTEST_SKIP() << "this vkms offers only one extent; nothing to change to";
  }
  ASSERT_EQ(backend->RequestModeChange(*other),
            DrmBackend::ModeChange::kExtent);
  ASSERT_TRUE(backend->MakeCurrent());
  ASSERT_TRUE(backend->ApplyPendingExtentChange());

  EXPECT_EQ(backend->RequestModeChange(*other), DrmBackend::ModeChange::kNone);
  EXPECT_FALSE(backend->ApplyPendingExtentChange());
}

// #762: the reported period comes from the mode's timings, not its vrefresh.
//
// Driven against whatever modes this host's vkms offers rather than a
// hardcoded one: find a mode where the exact period and the integer-vrefresh
// period actually differ, select it, and require the backend to report the
// exact figure. A default vkms has several -- 2560x1600 advertises vrefresh
// 60 and is really 59.9866Hz, a 3727ns gap -- but which ones exist is a
// kernel detail, so the case looks rather than assumes.
TEST_F(DrmBackendVkmsRefreshRate, ReportsThePeriodFromTheModesTimings) {
  drmModeConnector* connector = nullptr;
  drmModeRes* res = drmModeGetResources(display_->SharedDevice()->fd());
  ASSERT_NE(res, nullptr);
  for (int i = 0; i < res->count_connectors && connector == nullptr; ++i) {
    drmModeConnector* c =
        drmModeGetConnector(display_->SharedDevice()->fd(), res->connectors[i]);
    if (c != nullptr && c->connection == DRM_MODE_CONNECTED &&
        c->count_modes > 0) {
      connector = c;
    } else if (c != nullptr) {
      drmModeFreeConnector(c);
    }
  }
  ASSERT_NE(connector, nullptr) << "no connected vkms connector with modes";

  // A mode whose two periods disagree by enough to be unambiguous.
  std::string spec;
  uint32_t want_exact = 0;
  uint32_t want_integer = 0;
  for (int i = 0; i < connector->count_modes; ++i) {
    const drmModeModeInfo& m = connector->modes[i];
    if (m.vrefresh == 0 || m.clock == 0) {
      continue;
    }
    const uint32_t exact = ModeRefreshPeriodNs(m.clock, m.htotal, m.vtotal);
    const uint32_t integer = 1'000'000'000U / m.vrefresh;
    if (exact != 0 && exact != integer) {
      spec = std::to_string(m.hdisplay) + "x" + std::to_string(m.vdisplay) +
             "@" + std::to_string(m.vrefresh);
      want_exact = exact;
      want_integer = integer;
      break;
    }
  }
  drmModeFreeConnector(connector);
  drmModeFreeResources(res);
  if (spec.empty()) {
    GTEST_SKIP() << "every mode on this vkms has vrefresh exactly matching its "
                 << "timings; nothing to tell the two apart";
  }

  auto backend = BackendWithMode(spec);
  ASSERT_NE(backend, nullptr) << "could not select " << spec;

  EXPECT_EQ(backend->RefreshPeriodNs(), want_exact)
      << spec << ": expected the timing-derived period";
  EXPECT_NE(backend->RefreshPeriodNs(), want_integer)
      << spec << ": that is the integer-vrefresh period, which is the bug";

  // And the rate the view reports is just that period inverted, so the two
  // cannot disagree.
  EXPECT_NEAR(backend->RefreshRateHz(), 1e9 / static_cast<double>(want_exact),
              1e-6);
}

}  // namespace

// Own main rather than gtest_main: the shell's logging has to be started
// before any of it runs, and a compositor failure that logs nothing is very
// hard to tell apart from one that did not happen. IHS_LOG_LEVEL=debug shows
// the per-layer decisions PresentFramed makes.
// The EGL backend reports its repaint window to producers too.
TEST_F(PvHostVkms, CapabilitiesReportWhenTheShellTakesAFrame) {
  IhsPvCapabilities caps{};
  caps.struct_size = sizeof(caps);
  ASSERT_EQ(ihs_pv_query_capabilities(&caps), IHS_PV_OK);
  EXPECT_EQ(caps.latch_lead_ns, 5'000'000U) << "the default repaint window";
}

int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "drm_backend vkms test");
#if IHS_TEST_HAVE_CAPTURE
  std::cerr << "frame readback: available\n";
#else
  std::cerr << "frame readback: unavailable (built without blend2d, so drm-cxx "
               "has no capture module); pixel cases are compiled out\n";
#endif
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
