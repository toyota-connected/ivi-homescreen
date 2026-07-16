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

// Unit tests for the FlutterDesktopMessenger C ABI
// (flutter_desktop_messenger.cc).
//
// FlutterDesktopMessengerAddRef/Release/IsAvailable/Lock/Unlock are the five
// exported functions.  None of them require a live Wayland or Flutter engine
// connection — the messenger is a plain ref-counted struct with a mutex.
//
// NOTE: FlutterDesktopMessenger deletes itself when the ref-count reaches zero.
// Tests that call AddRef must eventually call the matching Release; tests that
// want to inspect the messenger after a Release call first AddRef it to 2 so
// the final Release leaves ref = 1 and does not trigger self-deletion.

#include <gtest/gtest.h>

#include "platform/homescreen/flutter_desktop_messenger.h"

// C ABI declarations (the definitions live in flutter_desktop_messenger.cc,
// which is compiled into homescreen_ut_shell).
extern FlutterDesktopMessengerRef FlutterDesktopMessengerAddRef(
    FlutterDesktopMessengerRef messenger);
extern void FlutterDesktopMessengerRelease(
    FlutterDesktopMessengerRef messenger);
extern bool FlutterDesktopMessengerIsAvailable(
    FlutterDesktopMessengerRef messenger);
extern FlutterDesktopMessengerRef FlutterDesktopMessengerLock(
    FlutterDesktopMessengerRef messenger);
extern void FlutterDesktopMessengerUnlock(FlutterDesktopMessengerRef messenger);

// RAII owner that owns a heap-allocated FlutterDesktopMessenger, holding it at
// ref-count 1.  On destruction it calls Release() which drops the count to 0
// and triggers self-deletion — this is the normal teardown path.
class MessengerOwner {
 public:
  MessengerOwner() : ptr_(new FlutterDesktopMessenger()) {
    // After construction ref_count_ is 0; bump it to 1 so the owner holds
    // the only reference.
    ptr_->AddRef();
  }
  ~MessengerOwner() {
    if (ptr_) {
      ptr_->Release();  // drops to 0 → self-delete
    }
  }
  [[nodiscard]] FlutterDesktopMessenger* get() const { return ptr_; }

 private:
  FlutterDesktopMessenger* ptr_;
};

// ---- AddRef ---------------------------------------------------------------

TEST(FlutterDesktopMessenger, AddRefReturnsSamePointer) {
  MessengerOwner owner;
  // AddRef returns the same pointer.  We must Release the extra ref.
  FlutterDesktopMessengerRef result =
      FlutterDesktopMessengerAddRef(owner.get());
  EXPECT_EQ(result, owner.get());
  // Drop the extra ref; owner.~MessengerOwner() drops the original.
  FlutterDesktopMessengerRelease(owner.get());
}

TEST(FlutterDesktopMessenger, DoubleAddRef_DoesNotDelete) {
  // AddRef twice, Release once → ref still > 0, object still alive.
  MessengerOwner owner;                         // ref = 1
  FlutterDesktopMessengerAddRef(owner.get());   // ref = 2
  FlutterDesktopMessengerRelease(owner.get());  // ref = 1 — no deletion
  // owner destructor releases once more (ref → 0, self-delete). No crash.
}

// ---- IsAvailable ----------------------------------------------------------

TEST(FlutterDesktopMessenger, IsAvailable_NoEngine_ReturnsFalse) {
  MessengerOwner owner;
  EXPECT_FALSE(FlutterDesktopMessengerIsAvailable(owner.get()));
}

TEST(FlutterDesktopMessenger, IsAvailable_WithEngine_ReturnsTrue) {
  MessengerOwner owner;
  // SetEngine with a non-null (but not dereferenced) pointer.
  // FlutterDesktopEngineState is forward-declared; we only need a non-null
  // sentinel — no member of it is accessed by IsAvailable.
  FlutterDesktopEngineState sentinel{};
  owner.get()->SetEngine(&sentinel);
  EXPECT_TRUE(FlutterDesktopMessengerIsAvailable(owner.get()));
  // Reset to avoid dangling; sentinel lives on the stack.
  owner.get()->SetEngine(nullptr);
}

// ---- Lock / Unlock --------------------------------------------------------

TEST(FlutterDesktopMessenger, Lock_ReturnsSamePointer) {
  MessengerOwner owner;
  FlutterDesktopMessengerRef locked = FlutterDesktopMessengerLock(owner.get());
  EXPECT_EQ(locked, owner.get());
  FlutterDesktopMessengerUnlock(owner.get());
}

TEST(FlutterDesktopMessenger, LockUnlock_NoCrash) {
  MessengerOwner owner;
  // Verify that a single lock/unlock cycle on the main thread does not
  // deadlock or crash.
  FlutterDesktopMessengerLock(owner.get());
  FlutterDesktopMessengerUnlock(owner.get());
}

TEST(FlutterDesktopMessenger, MultipleLockUnlock_NoCrash) {
  MessengerOwner owner;
  for (int i = 0; i < 3; ++i) {
    FlutterDesktopMessengerLock(owner.get());
    FlutterDesktopMessengerUnlock(owner.get());
  }
}
