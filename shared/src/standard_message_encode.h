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
 * The slice of Flutter's StandardMessageCodec needed to carry semantics action
 * arguments. Internal to ihs_shared: not installed, not exported, and not part
 * of the plugin ABI.
 *
 * Flutter vendors a complete codec in third_party, and this is deliberately
 * not it. That one is C++ over flutter::EncodableValue, and ihs_shared links
 * no Flutter library at all -- its whole premise is a flat C ABI with no C++
 * crossing the boundary (docs/PLUGIN_ABI.md). Pulling libflutter in to encode
 * three values would trade that for a handful of bytes.
 *
 * The cost of a second implementation is that it can drift from the one the
 * framework actually decodes with, silently, which is R-9 in the plan. The
 * mitigation is in the tests: everything encoded here is decoded again with
 * Flutter's own vendored serializer and compared, so a divergence fails rather
 * than reaching the framework as a dropped argument.
 */

#ifndef IHS_SHARED_SRC_STANDARD_MESSAGE_ENCODE_H_
#define IHS_SHARED_SRC_STANDARD_MESSAGE_ENCODE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ihs::codec {

/* A Dart String, which is what SemanticsAction.setText carries. */
std::vector<uint8_t> EncodeString(const std::string& value);

/*
 * A Dart Float64List. SemanticsAction.scrollToOffset takes one holding the
 * target offset as (dx, dy).
 */
std::vector<uint8_t> EncodeFloat64List(const std::vector<double>& values);

/*
 * A Dart Map<String, int>. SemanticsAction.setSelection takes one with "base"
 * and "extent". Values are written as int32, which is what the framework reads
 * a text offset as.
 */
std::vector<uint8_t> EncodeStringIntMap(
    const std::vector<std::pair<std::string, int32_t>>& entries);

}  // namespace ihs::codec

#endif  // IHS_SHARED_SRC_STANDARD_MESSAGE_ENCODE_H_
