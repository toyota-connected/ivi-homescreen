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

#ifndef IHS_SHARED_SRC_LOGGING_DLT_SINK_HPP_
#define IHS_SHARED_SRC_LOGGING_DLT_SINK_HPP_

#include "log_sink.hpp"

namespace ihs::dlt {

class LibDltLoader;

// Sink that forwards each record to libdlt via the loader. Emits nothing when
// libdlt is unavailable (the loader drops silently) or the record's DLT context
// never registered, so it is safe to keep in the sink list on a board with no
// dlt-daemon.
class DltSink final : public Sink {
 public:
  explicit DltSink(LibDltLoader& loader) noexcept : loader_(loader) {}

  void write(const LogRecord& record) noexcept override;

 private:
  LibDltLoader& loader_;
};

}  // namespace ihs::dlt

#endif  // IHS_SHARED_SRC_LOGGING_DLT_SINK_HPP_
