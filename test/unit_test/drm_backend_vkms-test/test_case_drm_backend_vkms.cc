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

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

  [[nodiscard]] uint32_t GetGlTextureName() const override { return tex_; }
  [[nodiscard]] int32_t GetGlTextureWidth() const override { return 16; }
  [[nodiscard]] int32_t GetGlTextureHeight() const override { return 16; }
  [[nodiscard]] uint32_t GetGlTextureBufferId() const override { return 0; }

  void OnScanoutRelease(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    released_.push_back(buffer_id);
  }
  void SetScanoutPlane(uint32_t plane_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    planes_.push_back(plane_id);
  }

  void set_texture(uint32_t tex) { tex_ = tex; }
  [[nodiscard]] int32_t width() const { return 16; }
  [[nodiscard]] int presents() const { return presents_; }

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
  int presents_{0};
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
    return true;
  }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return id_;
  }

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
      << "the fence handed back never signalled, so a producer waiting on it "
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
      << "a release eventfd never signalled; the host could not write to an fd "
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

  const std::vector<PlaneState> planes = ActivePlanes(card_.path);
  ASSERT_EQ(planes.size(), 2U)
      << "the view's two layers should be on two planes, and nothing else";
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
  std::vector<PlaneState> planes = ActivePlanes(card_.path);
  ASSERT_EQ(planes.size(), 1U);
  const uint32_t first_fb = planes[0].fb_id;

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
  ASSERT_EQ(planes.size(), 1U);
  EXPECT_NE(planes[0].fb_id, first_fb)
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

// The same through the GL compositor: the frame is composited, so it is not
// zero-copy, and it reaches the screen through the backend's own page flip.
class PvHostVkmsGl : public PvHostVkmsPlanes {
 protected:
  [[nodiscard]] const char* CompositorMode() const override { return "gl"; }
};

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

}  // namespace

// Own main rather than gtest_main: the shell's logging has to be started
// before any of it runs, and a compositor failure that logs nothing is very
// hard to tell apart from one that did not happen. IHS_LOG_LEVEL=debug shows
// the per-layer decisions PresentFramed makes.
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
