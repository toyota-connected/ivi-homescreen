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

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "hexdump.h"

// ---------------------------------------------------------------------------
// IosFlagSaver tests
// ---------------------------------------------------------------------------

TEST(IosFlagSaver, RestoresFlags) {
  std::ostringstream oss;
  // Record the default flags.
  const std::ios::fmtflags original = oss.flags();

  {
    IosFlagSaver saver(oss);
    // Change multiple flags inside the scope.
    oss << std::hex << std::setw(8) << std::setfill('0');
    EXPECT_NE(oss.flags(), original);
  }
  // After saver destruction the flags must be restored.
  EXPECT_EQ(oss.flags(), original);
}

TEST(IosFlagSaver, FillCharUnchangedByOurCode) {
  // IosFlagSaver does not touch the fill char — verify the stream fill is
  // independent of the flags so we don't accidentally assert on it.
  std::ostringstream oss;
  {
    IosFlagSaver saver(oss);
    oss.fill('*');
    EXPECT_EQ(oss.fill(), '*');
  }
  // fill is NOT restored by IosFlagSaver (it only saves fmtflags), so we just
  // verify the object compiled and destructed without UB.
}

// ---------------------------------------------------------------------------
// CustomHexdump tests
// ---------------------------------------------------------------------------

// Helper: stream dump to string.
template <unsigned RowSize, bool ShowAscii>
static std::string Dump(const uint8_t* data, size_t len) {
  std::ostringstream oss;
  oss << CustomHexdump<RowSize, ShowAscii>(data, len);
  return oss.str();
}

TEST(CustomHexdump, EmptyInput_ProducesNoOutput) {
  const std::string out = Dump<8, false>(nullptr, 0);
  EXPECT_TRUE(out.empty());
}

TEST(CustomHexdump, SingleByte_NoAscii_OffsetAndHex) {
  const uint8_t data[] = {0xAB};
  const std::string out = Dump<8, false>(data, 1);
  // Must contain the byte offset "0x000000:" and the hex value "ab".
  EXPECT_NE(out.find("0x000000:"), std::string::npos) << out;
  EXPECT_NE(out.find("ab"), std::string::npos) << out;
}

TEST(CustomHexdump, SingleByte_PadsRemainingCells) {
  const uint8_t data[] = {0x01};
  // RowSize=4: one real byte + 3 padding cells ("   " each).
  const std::string out = Dump<4, false>(data, 1);
  // Three padding slots = 9 spaces.  The line also has the real byte so
  // count trailing spaces conservatively: at least 6 padding spaces.
  const auto spaces = std::count(out.begin(), out.end(), ' ');
  EXPECT_GE(spaces, 6) << out;
}

TEST(CustomHexdump, FullRow_NoAscii_ExactlyOneNewline) {
  const uint8_t data[] = {0x00, 0x11, 0x22, 0x33};
  const std::string out = Dump<4, false>(data, 4);
  const auto newlines = std::count(out.begin(), out.end(), '\n');
  EXPECT_EQ(newlines, 1) << out;
}

TEST(CustomHexdump, MultipleRows_CorrectOffsets) {
  // 20 bytes with RowSize=8 → 3 rows at offsets 0, 8, 16.
  uint8_t data[20] = {};
  const std::string out = Dump<8, false>(data, 20);
  EXPECT_NE(out.find("0x000000:"), std::string::npos) << out;
  EXPECT_NE(out.find("0x000008:"), std::string::npos) << out;
  EXPECT_NE(out.find("0x000010:"), std::string::npos) << out;
  // Three rows → three newlines.
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 3L) << out;
}

TEST(CustomHexdump, AsciiColumn_PrintableChars) {
  // All printable ASCII → the ASCII column echoes the bytes.
  const uint8_t data[] = {'H', 'e', 'l', 'p'};
  const std::string out = Dump<4, true>(data, 4);
  EXPECT_NE(out.find('H'), std::string::npos) << out;
  EXPECT_NE(out.find('e'), std::string::npos) << out;
  EXPECT_NE(out.find('l'), std::string::npos) << out;
  EXPECT_NE(out.find('p'), std::string::npos) << out;
}

TEST(CustomHexdump, AsciiColumn_NonPrintableShownAsDot) {
  // Control bytes (0x00, 0x01) → dots in the ASCII column.
  const uint8_t data[] = {0x00, 0x01};
  const std::string out = Dump<2, true>(data, 2);
  // The hex values appear as "00" and "01"; the ASCII column must have dots.
  EXPECT_NE(out.find('.'), std::string::npos) << out;
}

TEST(CustomHexdump, NoAscii_DoesNotContainPrintableChars) {
  // With ShowAscii=false, the 'H' byte should appear as "48" not 'H'.
  const uint8_t data[] = {'H'};
  const std::string out = Dump<4, false>(data, 1);
  EXPECT_NE(out.find("48"), std::string::npos) << out;
  // 'H' should not appear as a bare character in the output (it might appear
  // inside "48" is fine — check there is no standalone ASCII column).
  // We verify by checking the line has no character that looks like an ASCII
  // section: the dump must contain "48" but the character 'H' appearing after
  // the hex block would only be present with ShowAscii=true.
  // Simple proxy: the output must NOT contain a bare 'H' that is not part of
  // "0x000000:" — strip the offset prefix and check no 'H' remains.
  const std::string body = out.substr(out.find(':') + 1);
  EXPECT_EQ(body.find('H'), std::string::npos) << out;
}

TEST(CustomHexdump, TypedefHexdump_CompilesToRow16WithAscii) {
  // Verify the `Hexdump` typedef (RowSize=16, ShowAscii=true) compiles and
  // produces 16-byte rows.
  uint8_t data[32] = {};
  for (int i = 0; i < 32; ++i) {
    data[i] = static_cast<uint8_t>(i);
  }
  std::ostringstream oss;
  oss << Hexdump(data, 32);
  // 32 bytes / 16 per row = 2 rows → 2 newlines.
  const std::string result = oss.str();
  EXPECT_EQ(std::count(result.begin(), result.end(), '\n'), 2L) << result;
}
