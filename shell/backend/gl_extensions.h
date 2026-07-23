// SPDX-FileCopyrightText: 2026 Toyota Connected North America
// SPDX-License-Identifier: Apache-2.0

#ifndef SHELL_BACKEND_GL_EXTENSIONS_H_
#define SHELL_BACKEND_GL_EXTENSIONS_H_

#include <cstring>
#include <string_view>

namespace ihs::gl {

// Whether @name appears as a whole, space-delimited token in a GL/EGL
// extension string. An exact match rather than a substring: a substring test
// accepts, say, GL_OES_EGL_image_external_essl3 as the base
// GL_OES_EGL_image_external and leads a caller to use a feature the driver does
// not have.
inline bool ExtensionSupported(const char* extensions, const char* name) {
  if (extensions == nullptr || name == nullptr) {
    return false;
  }
  const size_t name_len = std::strlen(name);
  const std::string_view sv(extensions);
  size_t pos = 0;
  while ((pos = sv.find(name, pos)) != std::string_view::npos) {
    const bool left_ok = (pos == 0) || (sv[pos - 1] == ' ');
    const size_t end = pos + name_len;
    const bool right_ok = (end == sv.size()) || (sv[end] == ' ');
    if (left_ok && right_ok) {
      return true;
    }
    pos = end;
  }
  return false;
}

}  // namespace ihs::gl

#endif  // SHELL_BACKEND_GL_EXTENSIONS_H_
