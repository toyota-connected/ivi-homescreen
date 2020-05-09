/*
 * Copyright 2020 Toyota Connected North America
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

#pragma once

#include <cctype>
#include <iomanip>
#include <ostream>

template <unsigned RowSize, bool ShowAscii>
struct CustomHexdump {
  CustomHexdump(const uint8_t* data, size_t length)
      : mData(data), mLength(length) {}
  const uint8_t* mData;
  const size_t mLength;
};

template <unsigned RowSize, bool ShowAscii>
std::ostream& operator<<(std::ostream& out,
                         const CustomHexdump<RowSize, ShowAscii>& dump) {
  out.fill('0');
  for (size_t i = 0; i < dump.mLength; i += RowSize) {
    out << "0x" << std::setw(6) << std::hex << i << ": ";
    for (size_t j = 0; j < RowSize; ++j) {
      if (i + j < dump.mLength) {
        out << std::hex << std::setw(2) << static_cast<int>(dump.mData[i + j])
            << " ";
      } else {
        out << "   ";
      }
    }

    out << " ";
    if (ShowAscii) {
      for (size_t j = 0; j < RowSize; ++j) {
        if (i + j < dump.mLength) {
          if (std::isprint(dump.mData[i + j])) {
            out << static_cast<char>(dump.mData[i + j]);
          } else {
            out << ".";
          }
        }
      }
    }
    out << std::endl;
  }
  return out;
}

typedef CustomHexdump<16, true> Hexdump;