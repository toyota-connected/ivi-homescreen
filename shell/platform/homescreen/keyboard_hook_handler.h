// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DESKTOP_KEYBOARD_HOOK_HANDLER_H
#define FLUTTER_SHELL_PLATFORM_DESKTOP_KEYBOARD_HOOK_HANDLER_H

#include <xkbcommon/xkbcommon.h>

#include "flutter_desktop_view_controller_state.h"
#include "view/flutter_view.h"

namespace flutter {

struct FlutterDesktopViewControllerState;

// Abstract class for handling keyboard input events.
class KeyboardHookHandler {
 public:
  virtual ~KeyboardHookHandler() = default;

  // A function for hooking into keyboard input.
  virtual void KeyboardHook(bool released,
                            xkb_keysym_t keysym,
                            uint32_t xkb_scancode,
                            uint32_t modifiers) = 0;

  // A function for hooking into unicode code point input.
  virtual void CharHook(unsigned int code_point) = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_DESKTOP_KEYBOARD_HOOK_HANDLER_H
