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

#include "dlt_sink.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "ring_slot.hpp"

#include "libdlt_loader.hpp"

namespace ihs::dlt {

void DltSink::write(const LogRecord& record) noexcept {
  // A record reaching a sink can now be longer than one ring slot: the drain
  // rejoins a message that was split across slots, so a console or file sink
  // prints it as one line. DLT does not get that. Its payload buffer is
  // libdlt's, sized by DLT_USER_BUF_MAX_SIZE inside a library we dlopen and
  // deliberately do not trust with its own bounds (see libdlt_loader's guard
  // scratch), and until this rejoin every record handed to it was bounded by
  // the slot. Clamping here keeps that bound exactly, so what DLT is asked to
  // carry does not change.
  if (record.text_len < kSlotTextCapacity) {
    loader_.emit(record.dlt_ctx, static_cast<int>(record.level), record.text);
    return;
  }
  // Say that it was clamped, the same way a record clipped on the way into a
  // slot says so. Silently handing DLT a shortened line would leave its reader
  // unable to tell one from a message that simply ended there.
  std::array<char, kSlotTextCapacity> clamped{};
  char mark[32];
  const int marked = std::snprintf(mark, sizeof(mark), "...+%zu",
                                   record.text_len - (clamped.size() - 1));
  const std::size_t mark_len =
      marked > 0 && static_cast<std::size_t>(marked) < sizeof(mark)
          ? static_cast<std::size_t>(marked)
          : 0;
  const std::size_t take = clamped.size() - 1 - mark_len;
  std::memcpy(clamped.data(), record.text, take);
  std::memcpy(clamped.data() + take, mark, mark_len);
  clamped[take + mark_len] = '\0';
  loader_.emit(record.dlt_ctx, static_cast<int>(record.level), clamped.data());
}

}  // namespace ihs::dlt
