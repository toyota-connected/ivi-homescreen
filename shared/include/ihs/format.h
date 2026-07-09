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

/*
 * ihs::format_to — a header-only formatting helper for building a log line into
 * a fixed buffer. C++ convenience only (not part of the C ABI). It resolves to
 * std::format_to_n on toolchains where <format> is usable (IHS_HAS_FORMAT_TO_N,
 * set by the build), and to snprintf otherwise; the format-string argument type
 * follows suit ("{}" placeholders vs printf-style). Both the logging bridge and
 * the shell's logger convenience use this one definition.
 */

#ifndef IHS_FORMAT_H_
#define IHS_FORMAT_H_

#ifdef __cplusplus

#include <cstddef>
#include <cstdio>
#include <utility>

#if defined(IHS_HAS_FORMAT_TO_N)
#include <format>
namespace ihs {
template <class... Args>
inline std::size_t format_to(char* buf,
                             std::size_t size,
                             std::format_string<Args...> fmt,
                             Args&&... args) {
  auto r = std::format_to_n(buf, size - 1, fmt, std::forward<Args>(args)...);
  *r.out = '\0';
  return static_cast<std::size_t>(r.size);
}
}  // namespace ihs
#else
namespace ihs {
template <class... Args>
inline std::size_t format_to(char* buf,
                             std::size_t size,
                             const char* fmt,
                             Args&&... args) {
  int n = std::snprintf(buf, size, fmt, std::forward<Args>(args)...);
  if (n < 0) {
    n = 0;
  }
  return static_cast<std::size_t>(n < static_cast<int>(size) ? n : size - 1);
}
}  // namespace ihs
#endif

#endif /* __cplusplus */

#endif /* IHS_FORMAT_H_ */
