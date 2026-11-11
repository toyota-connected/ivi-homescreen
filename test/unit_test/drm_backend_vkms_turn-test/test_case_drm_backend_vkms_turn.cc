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

// --drm-rotation on drm-kms-egl in a build without BUILD_COMPOSITOR (#848).
//
// Two ways to turn the picture there. The engine turns it itself through
// surface_transformation, but only under Skia and only for a picture that
// fills the panel; that is the engine's own drawing and needs an engine to
// see, so here it is checked only to be the path chosen. Everything else is
// rendered offscreen and turned at present by GlTurn, which these cases read
// back off the CRTC: a frame painted into the framebuffer the engine is given,
// the way the engine paints it, then presented.

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <GLES2/gl2.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "backend/drm_kms_egl/drm_backend.h"
#include "capture/png.hpp"
#include "capture/snapshot.hpp"
#include "display/drm_display.h"
#include "engine_switches.h"
#include "logging/logger.hpp"

namespace {

struct VkmsCard {
  std::string path;
  uint32_t mode_w{0};
  uint32_t mode_h{0};
};

// The first vkms card with a connected connector, and that connector's first
// mode, which is the one the backend picks.
VkmsCard FindVkms() {
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
    VkmsCard out;
    if (is_vkms) {
      if (drmModeRes* res = drmModeGetResources(fd); res != nullptr) {
        for (int c = 0; c < res->count_connectors && out.mode_w == 0; ++c) {
          drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
          if (conn == nullptr) {
            continue;
          }
          if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            out.path = path;
            out.mode_w = conn->modes[0].hdisplay;
            out.mode_h = conn->modes[0].vdisplay;
          }
          drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
      }
    }
    ::close(fd);
    if (!out.path.empty()) {
      return out;
    }
  }
  return {};
}

constexpr uint32_t kRed = 0x00FF0000u;
constexpr uint32_t kGreen = 0x0000FF00u;
constexpr uint32_t kBlue = 0x000000FFu;
constexpr uint32_t kWhite = 0x00FFFFFFu;
constexpr uint32_t kBlack = 0x00000000u;

// What the panel shows in the picture's quadrants (TL, TR, BL, BR) for a
// picture painted red, green, blue, white in that order, turned
// counter-clockwise as DRM_MODE_ROTATE_* turns a plane. Written out per angle
// rather than derived, so it does not share a mistake with the code under
// test.
std::array<uint32_t, 4> PanelQuadrants(const int degrees) {
  switch (degrees) {
    case 90:
      return {kGreen, kWhite, kRed, kBlue};
    case 180:
      return {kWhite, kBlue, kGreen, kRed};
    case 270:
      return {kBlue, kRed, kWhite, kGreen};
    default:
      return {kRed, kGreen, kBlue, kWhite};
  }
}

struct TurnCase {
  int degrees;
  bool impeller;
  // --width/--height; unset fills the panel.
  std::optional<uint32_t> w;
  std::optional<uint32_t> h;
};

std::string CaseName(const ::testing::TestParamInfo<TurnCase>& info) {
  const TurnCase& c = info.param;
  std::string name =
      "Rotate" + std::to_string(c.degrees) + (c.impeller ? "Impeller" : "Skia");
  if (c.w.has_value()) {
    name += "At" + std::to_string(*c.w) + "x" + std::to_string(*c.h);
  }
  return name;
}

class DrmBackendVkmsTurn : public ::testing::TestWithParam<TurnCase> {
 protected:
  void SetUp() override {
    card_ = FindVkms();
    if (card_.path.empty()) {
      GTEST_SKIP() << "no vkms card with a connected connector on this host "
                      "(sudo modprobe vkms)";
    }
    display_ = std::make_unique<DrmDisplay>(0, 0, 0.0, card_.path,
                                            /*no_seat=*/true);
    drm::Device* dev = display_->SharedDevice();
    if (dev == nullptr) {
      GTEST_SKIP() << "no DRM master on " << card_.path;
    }
    const TurnCase& c = GetParam();
    ihs::engine_switches::SetImpellerActive(c.impeller);

    DrmConfig cfg{card_.path, c.w, c.h, /*debug_backend=*/false};
    cfg.no_seat = true;
    cfg.disable_cursor = true;
    cfg.rotation = c.degrees;
    backend_ =
        DrmBackend::Create(cfg, display_->session(), dev, display_.get());
    ASSERT_NE(backend_, nullptr) << "DrmBackend::Create failed on vkms";
    display_->SetFlipHandler(&DrmBackend::UnifiedPageFlipHandler);
    display_->StartFlipReader();
    // What the engine is handed at startup; this is where the turn is chosen.
    config_ = backend_->GetRenderConfig();
  }

  void TearDown() override {
    backend_.reset();
    display_.reset();
    ihs::engine_switches::SetImpellerActive(false);
  }

  // Paints the picture into @p fbo the way the engine renders a framebuffer:
  // GL's y = 0 is the picture's bottom row.
  void PaintPicture(const GLuint fbo) {
    const auto w = static_cast<GLsizei>(backend_->width());
    const auto h = static_cast<GLsizei>(backend_->height());
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    const GLsizei hw = w / 2;
    const GLsizei hh = h / 2;
    const struct {
      GLint x;
      GLint y;
      GLsizei w;
      GLsizei h;
      uint32_t rgb;
    } fills[] = {{0, hh, hw, h - hh, kRed},
                 {hw, hh, w - hw, h - hh, kGreen},
                 {0, 0, hw, hh, kBlue},
                 {hw, 0, w - hw, hh, kWhite}};
    for (const auto& f : fills) {
      glScissor(f.x, f.y, f.w, f.h);
      glClearColor(static_cast<float>((f.rgb >> 16U) & 0xFFU) / 255.0F,
                   static_cast<float>((f.rgb >> 8U) & 0xFFU) / 255.0F,
                   static_cast<float>(f.rgb & 0xFFU) / 255.0F, 1.0F);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  // Presents and waits for the flip, so the CRTC scans out the frame.
  void PresentAndSettle() {
    const uint64_t flips = backend_->FlipsHandledForTest();
    ASSERT_TRUE(backend_->Present());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (backend_->FlipsHandledForTest() == flips &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GT(backend_->FlipsHandledForTest(), flips)
        << "the flip never landed";
  }

  drm::capture::Image Snapshot(const std::string& name) {
    auto shot = drm::capture::snapshot(backend_->device(), backend_->crtc_id());
    if (!shot) {
      ADD_FAILURE() << "snapshot failed: " << shot.error().message();
      return {};
    }
    drm::capture::Image image = std::move(shot.value());
    // IHS_TEST_CAPTURE_DIR keeps a PNG of the frame, to look at rather than
    // only assert on.
    if (const char* dir = std::getenv("IHS_TEST_CAPTURE_DIR"); dir != nullptr) {
      const std::string path = std::string(dir) + "/" + name + ".png";
      if (auto w = drm::capture::write_png(image, path); !w) {
        ADD_FAILURE() << "write_png: " << w.error().message();
      }
    }
    return image;
  }

  VkmsCard card_;
  std::unique_ptr<DrmDisplay> display_;
  std::unique_ptr<DrmBackend> backend_;
  FlutterRendererConfig config_{};
};

// Turned at present: the picture lands turned, centered when smaller than the
// panel, with black around it.
class DrmBackendVkmsTurnAtPresent : public DrmBackendVkmsTurn {};

TEST_P(DrmBackendVkmsTurnAtPresent, ThePictureIsTurnedOntoThePanel) {
  const TurnCase& c = GetParam();
  EXPECT_EQ(config_.open_gl.surface_transformation, nullptr)
      << "the engine was asked to turn a picture it cannot";

  ASSERT_TRUE(backend_->MakeCurrent());
  const GLuint fbo = backend_->TurnFramebufferForTest();
  ASSERT_NE(fbo, 0u) << "no framebuffer to render into";
  PaintPicture(fbo);
  ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
  PresentAndSettle();

  const drm::capture::Image image = Snapshot(CaseName({c, 0}));
  ASSERT_FALSE(image.empty());
  ASSERT_EQ(image.width(), card_.mode_w) << "the panel scans out at its mode";
  ASSERT_EQ(image.height(), card_.mode_h);
  const auto at = [&image](uint32_t x, uint32_t y) {
    return image.pixels()[static_cast<size_t>(y) * image.width() + x] &
           0x00FFFFFFu;
  };

  const DisplayRotation r = backend_->panel_rotation();
  const auto x0 = static_cast<uint32_t>(r.offset_x());
  const auto y0 = static_cast<uint32_t>(r.offset_y());
  const auto tw = static_cast<uint32_t>(r.rotated_w());
  const auto th = static_cast<uint32_t>(r.rotated_h());
  const auto want = PanelQuadrants(c.degrees);
  EXPECT_EQ(at(x0 + tw / 4, y0 + th / 4), want[0]) << "top-left";
  EXPECT_EQ(at(x0 + 3 * tw / 4, y0 + th / 4), want[1]) << "top-right";
  EXPECT_EQ(at(x0 + tw / 4, y0 + 3 * th / 4), want[2]) << "bottom-left";
  EXPECT_EQ(at(x0 + 3 * tw / 4, y0 + 3 * th / 4), want[3]) << "bottom-right";

  // The picture's own edges, exactly: its corner pixels are in it and the
  // ones just past them are not.
  EXPECT_EQ(at(x0, y0), want[0]);
  EXPECT_EQ(at(x0 + tw - 1, y0 + th - 1), want[3]);
  if (x0 > 0) {
    EXPECT_EQ(at(x0 - 1, y0 + th / 2), kBlack) << "left of the picture";
    EXPECT_EQ(at(x0 + tw, y0 + th / 2), kBlack) << "right of the picture";
  }
  if (y0 > 0) {
    EXPECT_EQ(at(x0 + tw / 2, y0 - 1), kBlack) << "above the picture";
    EXPECT_EQ(at(x0 + tw / 2, y0 + th), kBlack) << "below the picture";
  }
}

INSTANTIATE_TEST_SUITE_P(
    Cases,
    DrmBackendVkmsTurnAtPresent,
    ::testing::Values(
        // Impeller ignores the engine's turn, so it is turned here.
        TurnCase{90, true, std::nullopt, std::nullopt},
        TurnCase{180, true, std::nullopt, std::nullopt},
        TurnCase{270, true, std::nullopt, std::nullopt},
        // The engine's turn cannot center a smaller picture.
        TurnCase{90, false, 600, 400},
        TurnCase{180, false, 600, 400},
        TurnCase{270, false, 600, 400},
        TurnCase{90, true, 600, 400}),
    CaseName);

// The engine's own turn is chosen where it works, and nothing is turned at
// present then. Unturned, nothing is turned anywhere.
class DrmBackendVkmsTurnByEngine : public DrmBackendVkmsTurn {};

TEST_P(DrmBackendVkmsTurnByEngine, TheEngineTurnsItOrNothingDoes) {
  const TurnCase& c = GetParam();
  ASSERT_TRUE(backend_->MakeCurrent());
  EXPECT_EQ(backend_->TurnFramebufferForTest(), 0u)
      << "a frame the engine can turn is turned again at present";
  if (c.degrees == 0) {
    EXPECT_EQ(config_.open_gl.surface_transformation, nullptr);
  } else {
    EXPECT_NE(config_.open_gl.surface_transformation, nullptr)
        << "Skia filling the panel turns the picture itself";
  }
}

INSTANTIATE_TEST_SUITE_P(
    Cases,
    DrmBackendVkmsTurnByEngine,
    ::testing::Values(TurnCase{0, false, std::nullopt, std::nullopt},
                      TurnCase{0, true, std::nullopt, std::nullopt},
                      TurnCase{90, false, std::nullopt, std::nullopt},
                      TurnCase{180, false, std::nullopt, std::nullopt},
                      TurnCase{270, false, std::nullopt, std::nullopt}),
    CaseName);

}  // namespace

int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "drm_backend vkms turn test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
