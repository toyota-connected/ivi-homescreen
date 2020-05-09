// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_GLFW_KEYBOARD_HOOK_HANDLER_H_
#define FLUTTER_SHELL_PLATFORM_GLFW_KEYBOARD_HOOK_HANDLER_H_

#include "egl_window.h"

namespace flutter {

// Abstract class for handling keyboard input events.
class KeyboardHookHandler {
 public:
  virtual ~KeyboardHookHandler() = default;

  // A function for hooking into keyboard input.
  virtual void KeyboardHook(EglWindow* window,
                            int key,
                            int scancode,
                            int action,
                            int mods) = 0;

  // A function for hooking into unicode code point input.
  virtual void CharHook(EglWindow* window, unsigned int code_point) = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_GLFW_KEYBOARD_HOOK_HANDLER_H_
