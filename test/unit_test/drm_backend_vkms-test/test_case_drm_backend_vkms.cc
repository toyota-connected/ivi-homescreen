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

#include "backend/drm_kms_egl/drm_backend.h"
#include "backend/drm_kms_egl/drm_compositor.h"
#include "display/drm_display.h"
#include "logging/logger.hpp"
#include "platform/homescreen/platform_views/egl_dmabuf_import.h"
#include "view/compositor_surface_interface.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

extern "C" {
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
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
  // Overlay planes the card exposes. Zero on a default vkms: enable_overlay
  // defaults off, and without an overlay the driver probe disables the plane
  // compositor, so the framed path does not exist to be tested.
  int overlays{0};

  [[nodiscard]] bool ok() const { return !path.empty() && mode_w != 0; }
};

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
      for (int c = 0; c < res->count_connectors && out.mode_w == 0; ++c) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
        if (conn == nullptr) {
          continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
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

// A scanout-capable dma-buf filled with a flat colour.
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
    if (!fresh_) {
      return DmabufState::kNoNewFrame;
    }
    const int fd = buffer_->ExportFd();
    if (fd < 0) {
      return DmabufState::kNotScanoutCapable;
    }
    fresh_ = false;
    out->fd[0] = fd;
    out->fourcc = DRM_FORMAT_XRGB8888;
    out->modifier = DRM_FORMAT_MOD_LINEAR;
    out->width = buffer_->width();
    out->height = buffer_->height();
    out->plane_count = 1;
    out->offset[0] = 0;
    out->stride[0] = buffer_->stride();
    out->acquire_fence_fd = -1;  // filled by the CPU, already synced
    out->buffer_id = buffer_id_;
    delivered_++;
    return DmabufState::kFrame;
  }

  void OnScanoutRelease(uint32_t buffer_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    released_.push_back(buffer_id);
  }
  void SetScanoutPlane(uint32_t plane_id) override {
    const std::lock_guard<std::mutex> lock(mu_);
    planes_.push_back(plane_id);
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

  [[nodiscard]] int presents() const { return presents_; }
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

 private:
  const FlutterPlatformViewIdentifier id_;
  const GbmSolidBuffer* buffer_;
  uint32_t buffer_id_;
  int presents_{0};
  mutable std::mutex mu_;
  mutable bool fresh_{true};
  mutable int delivered_{0};
  std::vector<uint32_t> released_;
  std::vector<uint32_t> planes_;
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
    if (!card_.ok()) {
      GTEST_SKIP() << "no connected vkms card (sudo modprobe vkms)";
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

  // Paint the fixture's texture a flat colour, so a snapshot can say whether
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

  // The pixel at the centre of the CRTC, as 0xAARRGGBB.
  static uint32_t CentrePixel(const drm::capture::Image& img) {
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
    EXPECT_EQ(CentrePixel(image) & 0x00FFFFFFu, rgb)
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
  // set it to the whole framebuffer so the centre of the CRTC lands inside it
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
// colour the fake uploaded is the colour on screen at the centre of the
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
