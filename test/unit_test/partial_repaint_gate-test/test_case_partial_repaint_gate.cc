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

// Detects whether the forwarded engine switches request Impeller, which is
// what gates the Wayland/EGL backend's partial-repaint path. Pure function, so
// no engine is needed.

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "backend/wayland_egl/partial_repaint_gate.h"

TEST(PartialRepaintGate, NoSwitchesIsSkia) {
  EXPECT_FALSE(EngineSwitchesEnableImpeller({}));
}

TEST(PartialRepaintGate, UnrelatedSwitchesIsSkia) {
  EXPECT_FALSE(EngineSwitchesEnableImpeller({"--foo", "--disable-impeller"}));
}

TEST(PartialRepaintGate, BareFlagEnablesImpeller) {
  EXPECT_TRUE(EngineSwitchesEnableImpeller({"--enable-impeller"}));
}

TEST(PartialRepaintGate, ExplicitTrueEnablesImpeller) {
  EXPECT_TRUE(EngineSwitchesEnableImpeller({"--enable-impeller=true"}));
}

TEST(PartialRepaintGate, ExplicitFalseStaysSkia) {
  EXPECT_FALSE(EngineSwitchesEnableImpeller({"--enable-impeller=false"}));
  EXPECT_FALSE(EngineSwitchesEnableImpeller({"--enable-impeller=0"}));
}

TEST(PartialRepaintGate, FoundAmongOtherSwitches) {
  EXPECT_TRUE(EngineSwitchesEnableImpeller(
      {"--disable-dart-dev", "--enable-impeller", "--verbose-logging"}));
}

TEST(PartialRepaintGate, PrefixIsNotAMatch) {
  // A longer switch that merely starts with the key must not trip the gate.
  EXPECT_FALSE(EngineSwitchesEnableImpeller({"--enable-impellerish"}));
}

TEST(PartialRepaintGate, LastMatchingSwitchWins) {
  EXPECT_TRUE(EngineSwitchesEnableImpeller(
      {"--enable-impeller=false", "--enable-impeller"}));
  EXPECT_FALSE(EngineSwitchesEnableImpeller(
      {"--enable-impeller", "--enable-impeller=false"}));
}
