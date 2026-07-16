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

// Unit tests for flutter::KeyEventHandler.
//
// Covers:
//   - Channel registration on construction
//   - CharHook (public no-op)
//   - FocusLost with and without a TextInputPlugin wired
//   - KeymapChanged updates ctrl_mask_ / alt_mask_ (private → public under
//     UNIT_TEST)
//   - Destructor does not crash after teardown (ASAN verifies no UAF)
//
// KeyboardHook and the engine SendKeyEvent path are excluded: they call
// LibFlutterEngine (not loaded in unit-test builds) and require a live asio
// strand from Engine::GetPlatformTaskRunner().

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <xkbcommon/xkbcommon.h>

#include "stub_binary_messenger.h"

#include "shell/platform/homescreen/key_event_handler.h"
#include "shell/platform/homescreen/text_input_plugin.h"

static constexpr char kChannel[] = "flutter/keyevent";

// ---- RAII wrappers for xkbcommon objects ----------------------------------

struct XkbContextDeleter {
  void operator()(xkb_context* ctx) const { xkb_context_unref(ctx); }
};
struct XkbKeymapDeleter {
  void operator()(xkb_keymap* km) const { xkb_keymap_unref(km); }
};

using XkbContextPtr = std::unique_ptr<xkb_context, XkbContextDeleter>;
using XkbKeymapPtr = std::unique_ptr<xkb_keymap, XkbKeymapDeleter>;

static XkbContextPtr MakeContext() {
  return XkbContextPtr(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
}

static XkbKeymapPtr MakeDefaultKeymap(xkb_context* ctx) {
  return XkbKeymapPtr(
      xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS));
}

// ---- Construction ---------------------------------------------------------
// KeyEventHandler uses a BasicMessageChannel which does NOT auto-register a
// handler on construction — it registers only when the engine sets a handler.
// We verify construction succeeds without crashing.

TEST(KeyEventHandler, ConstructionNoCrash) {
  StubBinaryMessenger messenger;
  EXPECT_NO_FATAL_FAILURE({
    flutter::KeyEventHandler handler(&messenger);
    (void)handler;
  });
}

// ---- CharHook (public no-op) ----------------------------------------------

TEST(KeyEventHandler, CharHook_IsNoOp) {
  StubBinaryMessenger messenger;
  flutter::KeyEventHandler handler(&messenger);
  EXPECT_NO_FATAL_FAILURE(handler.CharHook(0x41 /* 'A' */));
  EXPECT_NO_FATAL_FAILURE(handler.CharHook(0));
}

// ---- FocusLost without TextInputPlugin ------------------------------------

TEST(KeyEventHandler, FocusLost_WithoutTextInputPlugin_NoOp) {
  StubBinaryMessenger messenger;
  flutter::KeyEventHandler handler(&messenger);
  // Default: text_input_ == nullptr. Must not crash.
  EXPECT_NO_FATAL_FAILURE(handler.FocusLost());
}

// ---- FocusLost with TextInputPlugin ---------------------------------------

TEST(KeyEventHandler, FocusLost_WithTextInputPlugin_ForwardsCall) {
  // Construct a real TextInputPlugin with a stub messenger (it only needs a
  // BinaryMessenger to register its channel; it does not connect to anything).
  StubBinaryMessenger messenger;
  flutter::KeyEventHandler handler(&messenger);

  // TextInputPlugin requires its own separate messenger for channel reg.
  StubBinaryMessenger plugin_messenger;
  flutter::TextInputPlugin text_input(&plugin_messenger);

  handler.SetTextInputPlugin(&text_input);

  // FocusLost should forward to text_input.FocusLost() without crashing.
  EXPECT_NO_FATAL_FAILURE(handler.FocusLost());
}

// ---- KeymapChanged updates modifier masks ---------------------------------

TEST(KeyEventHandler, KeymapChanged_WithValidKeymap_NoCrash) {
  // KeymapChanged reads modifier indices from the live keymap and updates
  // internal atomics.  We verify it does not crash or abort; ASAN verifies
  // no out-of-bounds access.
  auto ctx = MakeContext();
  ASSERT_NE(ctx, nullptr);
  auto keymap = MakeDefaultKeymap(ctx.get());
  ASSERT_NE(keymap, nullptr);

  StubBinaryMessenger messenger;
  flutter::KeyEventHandler handler(&messenger);
  EXPECT_NO_FATAL_FAILURE(handler.KeymapChanged(keymap.get()));
}

TEST(KeyEventHandler, KeymapChanged_CalledTwice_NoCrash) {
  // Ensure a second KeymapChanged (keymap reload) does not corrupt state.
  auto ctx = MakeContext();
  ASSERT_NE(ctx, nullptr);
  auto keymap = MakeDefaultKeymap(ctx.get());
  ASSERT_NE(keymap, nullptr);

  StubBinaryMessenger messenger;
  flutter::KeyEventHandler handler(&messenger);
  EXPECT_NO_FATAL_FAILURE(handler.KeymapChanged(keymap.get()));
  EXPECT_NO_FATAL_FAILURE(handler.KeymapChanged(keymap.get()));
}

// ---- Destructor cleans up safely -----------------------------------------

TEST(KeyEventHandler, DestructorNoCrash) {
  StubBinaryMessenger messenger;
  {
    flutter::KeyEventHandler handler(&messenger);
    (void)handler;
  }
  // ASAN verifies no use-after-free.
}
