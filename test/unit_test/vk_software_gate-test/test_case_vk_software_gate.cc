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

#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "backend/drm_kms_vulkan/device_caps.h"

namespace {

constexpr const char* kEnv = "IVI_DRMVK_ALLOW_SOFTWARE";

// Saves and restores the variable, so a test cannot leak its setting into the
// rest of the process -- the backend reads it at every device selection.
class SoftwareGate : public ::testing::Test {
 protected:
  void SetUp() override {
    if (const char* v = std::getenv(kEnv); v != nullptr) {
      saved_ = v;
    }
    ::unsetenv(kEnv);
  }
  void TearDown() override {
    if (saved_) {
      ::setenv(kEnv, saved_->c_str(), 1);
    } else {
      ::unsetenv(kEnv);
    }
  }
  static void Set(const char* value) { ::setenv(kEnv, value, 1); }

 private:
  std::optional<std::string> saved_;
};

}  // namespace

// The default is what protects a mis-provisioned board: refusing a software
// device is what turns a missing GPU driver into a loud failure at start rather
// than a car UI running at software speed.
TEST_F(SoftwareGate, SoftwareIsRefusedWhenNothingAsksForIt) {
  EXPECT_FALSE(drm_kms_vulkan::AllowSoftwareRenderer());
}

TEST_F(SoftwareGate, OnlyExactlyOneOptsIn) {
  Set("1");
  EXPECT_TRUE(drm_kms_vulkan::AllowSoftwareRenderer());

  // Anything else is not an opt-in. A truthy-looking value that silently
  // enabled a software renderer would defeat the point of the default.
  for (const char* v :
       {"0", "yes", "true", "TRUE", "on", "", " 1", "1 ", "11"}) {
    Set(v);
    EXPECT_FALSE(drm_kms_vulkan::AllowSoftwareRenderer())
        << "IVI_DRMVK_ALLOW_SOFTWARE=\"" << v << "\" must not opt in";
  }
}
