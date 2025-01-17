// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/fml/string_conversion.h"

#include <codecvt>
#include <locale>
#include <sstream>
#include <string>

#include "flutter/fml/build_config.h"

#if defined(FML_OS_WIN)
// TODO(naifu): https://github.com/flutter/flutter/issues/98074
// Eliminate this workaround for a link error on Windows when the underlying
// bug is fixed.
std::locale::id std::codecvt<char16_t, char, _Mbstatet>::id;
#endif  // defined(FML_OS_WIN)

namespace fml {

std::string Join(const std::vector<std::string>& vec, const char* delimiter) {
  std::stringstream res;
  for (size_t i = 0; i < vec.size(); ++i) {
    res << vec[i];
    if (i < vec.size() - 1) {
      res << delimiter;
    }
  }
  return res.str();
}

std::string Utf16ToUtf8(const std::u16string_view& string) {
  std::string result;
  result.reserve(string.size() * 2);  // Reserve enough space for the conversion
  for (const char16_t ch : string) {
    if (ch <= 0x7F) {
      result.push_back(static_cast<char>(ch));
    } else if (ch <= 0x7FF) {
      result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
      result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
      result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
  }
  return result;
}

std::u16string Utf8ToUtf16(const std::string_view& string) {
  std::u16string result;
  result.reserve(string.size());  // Reserve enough space for the conversion
  for (size_t i = 0; i < string.size(); ++i) {
    char16_t ch = 0;
    if ((string[i] & 0x80) == 0) {
      ch = static_cast<char16_t>(string[i]);
    } else if ((string[i] & 0xE0) == 0xC0) {
      ch = static_cast<char16_t>((string[i] & 0x1F) << 6);
      ch = ch | static_cast<char16_t>(string[++i] & 0x3F);
    } else if ((string[i] & 0xF0) == 0xE0) {
      ch = static_cast<char16_t>((string[i] & 0x0F) << 12);
      ch = ch | static_cast<char16_t>((string[++i] & 0x3F) << 6);
      ch = ch | static_cast<char16_t>(string[++i] & 0x3F);
    }
    result.push_back(ch);
  }
  return result;
}

}  // namespace fml
